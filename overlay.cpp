// overlay.cpp — Browser overlay with adjustable opacity, screenshot exclusion,
// and OpenRouter AI analysis.
//
// Build:  see CMakeLists.txt
// Deps:   X11, Xcomposite, Xrender, Cairo, cairo-xlib, libcurl, libpng, pthread
//
// How "invisible to screenshot" works:
//   The overlay window is unmapped (hidden) for 100 ms before XGetImage captures
//   the root window, then remapped.  Because XGetImage reads the composited
//   framebuffer AFTER the window is gone, the overlay never appears in the grab.

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xrender.h>

#include <cairo/cairo.h>
#include <cairo/cairo-xlib.h>

#include <curl/curl.h>
#include <png.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ─── Hard-coded config ────────────────────────────────────────────────────────
static const char* OPENROUTER_API_KEY = "sk-or-v1-YOUR_API_KEY_HERE";
static const char* OPENROUTER_MODEL   = "google/gemini-2.0-flash-001";
static const char* OPENROUTER_URL     = "https://openrouter.ai/api/v1/chat/completions";

// ─── Overlay dimensions ───────────────────────────────────────────────────────
static constexpr int OW = 440;   // overlay width
static constexpr int OH = 620;   // overlay height

// ─── App state ────────────────────────────────────────────────────────────────
struct Rect { int x, y, w, h; };

struct AppState {
    Display*         dpy    = nullptr;
    Window           win    = 0;
    int              screen = 0;
    cairo_surface_t* surf   = nullptr;
    cairo_t*         cr     = nullptr;

    double opacity            = 0.88;
    bool   dragging_slider    = false;

    std::atomic<bool> capturing{false};
    std::atomic<bool> waiting{false};

    std::string  response_text;
    std::mutex   response_mtx;

    bool needs_redraw = true;

    // UI hit-areas
    Rect slider_track  = {20, 50, OW - 40, 20};
    Rect btn_capture   = {20, 88, 200, 36};
};

// ─── Base64 ───────────────────────────────────────────────────────────────────
static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string base64_encode(const uint8_t* data, size_t len)
{
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t b = (uint32_t)data[i] << 16;
        if (i + 1 < len) b |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) b |= data[i + 2];
        out += B64[(b >> 18) & 0x3f];
        out += B64[(b >> 12) & 0x3f];
        out += (i + 1 < len) ? B64[(b >> 6) & 0x3f] : '=';
        out += (i + 2 < len) ? B64[b & 0x3f]        : '=';
    }
    return out;
}

// ─── PNG encode ───────────────────────────────────────────────────────────────
struct PngBuf { std::vector<uint8_t> data; };

static void png_write_cb(png_structp png, png_bytep d, png_size_t n) {
    static_cast<PngBuf*>(png_get_io_ptr(png))->data.insert(
        static_cast<PngBuf*>(png_get_io_ptr(png))->data.end(), d, d + n);
}

static std::vector<uint8_t> ximage_to_png(XImage* img)
{
    PngBuf buf;
    png_structp png = png_create_write_struct(
        PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) return {};
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, nullptr); return {}; }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        return {};
    }

    png_set_write_fn(png, &buf, png_write_cb, nullptr);
    png_set_IHDR(png, info,
        img->width, img->height, 8,
        PNG_COLOR_TYPE_RGB,
        PNG_INTERLACE_NONE,
        PNG_COMPRESSION_TYPE_DEFAULT,
        PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    std::vector<uint8_t> row(img->width * 3);
    for (int y = 0; y < img->height; y++) {
        for (int x = 0; x < img->width; x++) {
            unsigned long px = XGetPixel(img, x, y);
            row[x * 3 + 0] = (px >> 16) & 0xff;  // R
            row[x * 3 + 1] = (px >>  8) & 0xff;  // G
            row[x * 3 + 2] =  px        & 0xff;   // B
        }
        png_write_row(png, row.data());
    }
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    return buf.data;
}

// ─── Screenshot (overlay hidden during capture) ───────────────────────────────
static std::vector<uint8_t> take_screenshot(AppState& app)
{
    Display* dpy  = app.dpy;
    int      scr  = app.screen;
    Window   root = RootWindow(dpy, scr);
    int W = DisplayWidth(dpy, scr);
    int H = DisplayHeight(dpy, scr);

    // Hide so the overlay doesn't show in the grab
    XUnmapWindow(dpy, app.win);
    XSync(dpy, False);
    struct timespec ts = {0, 120'000'000};   // 120 ms — let compositor repaint
    nanosleep(&ts, nullptr);

    XImage* img = XGetImage(dpy, root, 0, 0, W, H, AllPlanes, ZPixmap);

    // Restore overlay
    XMapWindow(dpy, app.win);
    XRaiseWindow(dpy, app.win);
    XSync(dpy, False);

    if (!img) return {};
    auto png = ximage_to_png(img);
    XDestroyImage(img);
    return png;
}

// ─── OpenRouter API call ──────────────────────────────────────────────────────
static size_t curl_write_cb(char* ptr, size_t sz, size_t nmemb, void* ud)
{
    static_cast<std::string*>(ud)->append(ptr, sz * nmemb);
    return sz * nmemb;
}

static std::string send_to_openrouter(const std::vector<uint8_t>& png)
{
    std::string b64 = base64_encode(png.data(), png.size());

    // Build request body
    std::string body =
        "{\"model\":\"" + std::string(OPENROUTER_MODEL) + "\","
        "\"messages\":[{\"role\":\"user\",\"content\":["
        "{\"type\":\"text\",\"text\":\"Describe what is visible in this browser screenshot. "
        "Be concise but thorough — mention the site, key content, and any notable UI elements.\"},"
        "{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64," + b64 + "\"}}"
        "]}]}";

    CURL* curl = curl_easy_init();
    if (!curl) return "[curl init failed]";

    struct curl_slist* hdrs = nullptr;
    std::string auth = "Authorization: Bearer " + std::string(OPENROUTER_API_KEY);
    hdrs = curl_slist_append(hdrs, auth.c_str());
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    hdrs = curl_slist_append(hdrs, "HTTP-Referer: https://github.com/overlay-tool");
    hdrs = curl_slist_append(hdrs, "X-Title: Browser Overlay");

    std::string resp;
    curl_easy_setopt(curl, CURLOPT_URL,           OPENROUTER_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    hdrs);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       60L);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK)
        return std::string("[curl error] ") + curl_easy_strerror(rc);

    // ── Simple extraction of the first "content" string value ──
    // Finds: "content":"<text>"  (handles escaped chars)
    auto find_content = [](const std::string& json) -> std::string {
        size_t p = json.find("\"content\"");
        if (p == std::string::npos) return json;   // fallback: return raw
        p = json.find('"', p + 9);                 // find opening quote
        if (p == std::string::npos) return json;

        // Check if value is an array (vision models sometimes return [{...}])
        size_t vstart = json.find_first_not_of(" \t\r\n", p + 1);
        if (vstart != std::string::npos && json[vstart] == '[') {
            // Find nested "text" field inside the array
            size_t tp = json.find("\"text\"", vstart);
            if (tp == std::string::npos) return json;
            p = json.find('"', tp + 6);
            if (p == std::string::npos) return json;
        }

        p++;  // skip opening quote
        std::string out;
        while (p < json.size()) {
            if (json[p] == '\\' && p + 1 < json.size()) {
                char nc = json[p + 1];
                if      (nc == 'n')  out += '\n';
                else if (nc == 't')  out += '\t';
                else if (nc == '"')  out += '"';
                else if (nc == '\\') out += '\\';
                else                 out += nc;
                p += 2;
            } else if (json[p] == '"') {
                break;
            } else {
                out += json[p++];
            }
        }
        return out.empty() ? json : out;
    };

    return find_content(resp);
}

// ─── Window opacity (via compositor hint) ─────────────────────────────────────
static void apply_opacity(AppState& app)
{
    // _NET_WM_WINDOW_OPACITY is a 32-bit cardinal where 0xFFFFFFFF = fully opaque
    uint32_t val = static_cast<uint32_t>(app.opacity * 0xFFFFFFFFu);
    Atom atom = XInternAtom(app.dpy, "_NET_WM_WINDOW_OPACITY", False);
    XChangeProperty(app.dpy, app.win, atom, XA_CARDINAL, 32,
                    PropModeReplace,
                    reinterpret_cast<unsigned char*>(&val), 1);
}

// ─── Drawing ──────────────────────────────────────────────────────────────────
static void draw(AppState& app)
{
    cairo_t* cr = app.cr;
    const double W = OW, H = OH, R = 10.0;

    // ── Background (semi-transparent dark panel) ──
    cairo_save(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0.07, 0.07, 0.11, 0.93);
    cairo_paint(cr);
    cairo_restore(cr);

    // ── Rounded border ──
    cairo_new_path(cr);
    cairo_arc(cr,   R,   R, R, M_PI,       1.5 * M_PI);
    cairo_arc(cr, W-R,   R, R, 1.5*M_PI,  0);
    cairo_arc(cr, W-R, H-R, R, 0,          0.5*M_PI);
    cairo_arc(cr,   R, H-R, R, 0.5*M_PI,  M_PI);
    cairo_close_path(cr);
    cairo_set_source_rgba(cr, 0.25, 0.55, 1.0, 0.55);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);

    // ── Title ──
    cairo_select_font_face(cr, "Sans",
        CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, 13.5);
    cairo_set_source_rgba(cr, 0.88, 0.91, 1.0, 1.0);
    cairo_move_to(cr, 18, 27);
    cairo_show_text(cr, "Browser Overlay  —  AI Analyzer");

    // ── Opacity label ──
    char lbl[40];
    snprintf(lbl, sizeof(lbl), "Opacity: %d%%", (int)(app.opacity * 100));
    cairo_set_font_size(cr, 11);
    cairo_set_source_rgba(cr, 0.65, 0.68, 0.85, 1.0);
    cairo_move_to(cr, 18, 46);
    cairo_show_text(cr, lbl);

    // ── Slider track ──
    auto& sl = app.slider_track;
    cairo_set_source_rgba(cr, 0.18, 0.18, 0.28, 1.0);
    cairo_rectangle(cr, sl.x, sl.y + 5, sl.w, 8);
    cairo_fill(cr);

    // ── Slider fill ──
    double fill = sl.w * app.opacity;
    cairo_set_source_rgba(cr, 0.25, 0.55, 1.0, 1.0);
    cairo_rectangle(cr, sl.x, sl.y + 5, fill, 8);
    cairo_fill(cr);

    // ── Slider knob ──
    cairo_arc(cr, sl.x + fill, sl.y + 9, 7.5, 0, 2 * M_PI);
    cairo_set_source_rgba(cr, 0.85, 0.92, 1.0, 1.0);
    cairo_fill(cr);

    // ── Capture button ──
    bool busy = app.capturing || app.waiting;
    auto& btn = app.btn_capture;
    cairo_set_source_rgba(cr,
        busy ? 0.28 : 0.18,
        busy ? 0.28 : 0.48,
        busy ? 0.38 : 0.92, 1.0);
    // Rounded rect for button
    double br = 6.0;
    cairo_new_path(cr);
    cairo_arc(cr, btn.x+br,       btn.y+br,       br, M_PI,      1.5*M_PI);
    cairo_arc(cr, btn.x+btn.w-br, btn.y+br,       br, 1.5*M_PI, 0);
    cairo_arc(cr, btn.x+btn.w-br, btn.y+btn.h-br, br, 0,         0.5*M_PI);
    cairo_arc(cr, btn.x+br,       btn.y+btn.h-br, br, 0.5*M_PI,  M_PI);
    cairo_close_path(cr);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
    cairo_set_font_size(cr, 12);
    cairo_move_to(cr, btn.x + 14, btn.y + 23);
    if (app.capturing)
        cairo_show_text(cr, "Capturing screen...");
    else if (app.waiting)
        cairo_show_text(cr, "Waiting for AI...");
    else
        cairo_show_text(cr, "Screenshot & Analyze");

    // ── Response label ──
    cairo_set_source_rgba(cr, 0.45, 0.65, 1.0, 1.0);
    cairo_set_font_size(cr, 10);
    cairo_move_to(cr, 18, 142);
    cairo_show_text(cr, "AI Response");

    // ── Response box ──
    int box_x = 16, box_y = 148;
    int box_w = OW - 32, box_h = OH - 165;
    cairo_set_source_rgba(cr, 0.10, 0.10, 0.17, 1.0);
    cairo_rectangle(cr, box_x, box_y, box_w, box_h);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0.30, 0.30, 0.50, 0.7);
    cairo_rectangle(cr, box_x, box_y, box_w, box_h);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    // ── Response text (word-wrapped) ──
    std::string resp;
    {
        std::lock_guard<std::mutex> lk(app.response_mtx);
        resp = app.response_text;
    }

    cairo_set_font_size(cr, 10.5);
    cairo_select_font_face(cr, "Sans",
        CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

    if (resp.empty()) {
        cairo_set_source_rgba(cr, 0.38, 0.38, 0.52, 1.0);
        cairo_move_to(cr, box_x + 10, box_y + 20);
        cairo_show_text(cr, "Press the button above to capture and analyze.");
    } else {
        cairo_set_source_rgba(cr, 0.84, 0.86, 0.96, 1.0);

        int tx = box_x + 8;
        int ty = box_y + 15;
        int max_x_width = box_w - 16;
        int line_h = 14;
        int max_ty = box_y + box_h - 8;

        // Word-wrap with newline handling
        std::string line;
        cairo_text_extents_t ext;

        auto flush_line = [&]() {
            if (ty >= max_ty) return;
            cairo_move_to(cr, tx, ty);
            cairo_show_text(cr, line.c_str());
            ty += line_h;
            line.clear();
        };

        // Split on newlines first, then word-wrap each paragraph
        std::istringstream para_stream(resp);
        std::string paragraph;
        bool first_para = true;
        while (std::getline(para_stream, paragraph)) {
            if (!first_para) {
                flush_line();  // blank line between paragraphs
                if (ty < max_ty) ty += 2;
            }
            first_para = false;
            line.clear();

            std::istringstream word_stream(paragraph);
            std::string word;
            while (word_stream >> word) {
                std::string candidate = line.empty() ? word : (line + " " + word);
                cairo_text_extents(cr, candidate.c_str(), &ext);
                if (ext.width > max_x_width && !line.empty()) {
                    flush_line();
                    line = word;
                } else {
                    line = candidate;
                }
            }
        }
        if (!line.empty()) flush_line();
    }

    XFlush(app.dpy);
}

// ─── X11 window setup ─────────────────────────────────────────────────────────
static void setup_hints(AppState& app)
{
    Display* dpy = app.dpy;
    Window   win = app.win;

    // Always on top + skip taskbar/pager
    Atom wm_state       = XInternAtom(dpy, "_NET_WM_STATE",            False);
    Atom state_above    = XInternAtom(dpy, "_NET_WM_STATE_ABOVE",      False);
    Atom state_skip_tb  = XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False);
    Atom state_skip_pg  = XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER",   False);
    Atom states[3]      = { state_above, state_skip_tb, state_skip_pg };
    XChangeProperty(dpy, win, wm_state, XA_ATOM, 32,
                    PropModeReplace, (unsigned char*)states, 3);

    // Window type: utility (no decorations, no taskbar entry)
    Atom wm_type    = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE",         False);
    Atom type_util  = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False);
    XChangeProperty(dpy, win, wm_type, XA_ATOM, 32,
                    PropModeReplace, (unsigned char*)&type_util, 1);

    // Input hint = False → WM will not give keyboard focus to this window
    // (browser keeps focus; overlay is click-only for its own buttons)
    XWMHints* wm_hints = XAllocWMHints();
    wm_hints->flags         = InputHint | StateHint;
    wm_hints->input         = False;
    wm_hints->initial_state = NormalState;
    XSetWMHints(dpy, win, wm_hints);
    XFree(wm_hints);

    // Initial opacity
    apply_opacity(app);
}

// ─── Main ─────────────────────────────────────────────────────────────────────
int main()
{
    curl_global_init(CURL_GLOBAL_DEFAULT);

    AppState app;
    app.dpy = XOpenDisplay(nullptr);
    if (!app.dpy) {
        fprintf(stderr, "Cannot open X display. Is DISPLAY set?\n");
        return 1;
    }
    app.screen = DefaultScreen(app.dpy);

    // Need a 32-bit ARGB visual for per-pixel transparency
    XVisualInfo vinfo{};
    if (!XMatchVisualInfo(app.dpy, app.screen, 32, TrueColor, &vinfo)) {
        fprintf(stderr, "No 32-bit ARGB visual available (compositing disabled?)\n");
        return 1;
    }

    Colormap cmap = XCreateColormap(
        app.dpy, RootWindow(app.dpy, app.screen), vinfo.visual, AllocNone);

    XSetWindowAttributes xswa{};
    xswa.colormap     = cmap;
    xswa.border_pixel = 0;
    xswa.background_pixel = 0;

    app.win = XCreateWindow(
        app.dpy,
        RootWindow(app.dpy, app.screen),
        40, 40,           // initial position
        OW, OH, 0,        // size, border
        vinfo.depth, InputOutput, vinfo.visual,
        CWColormap | CWBorderPixel | CWBackPixel,
        &xswa);

    XSelectInput(app.dpy, app.win,
        ExposureMask | ButtonPressMask | ButtonReleaseMask |
        PointerMotionMask | StructureNotifyMask);

    setup_hints(app);

    XStoreName(app.dpy, app.win, "Browser Overlay");

    Atom wm_delete = XInternAtom(app.dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(app.dpy, app.win, &wm_delete, 1);

    XMapWindow(app.dpy, app.win);
    XSync(app.dpy, False);

    // Cairo
    app.surf = cairo_xlib_surface_create(
        app.dpy, app.win, vinfo.visual, OW, OH);
    app.cr = cairo_create(app.surf);

    // ── Event loop ──────────────────────────────────────────────────────────
    bool running = true;
    XEvent ev;

    while (running) {
        while (XPending(app.dpy)) {
            XNextEvent(app.dpy, &ev);

            if (ev.type == ClientMessage &&
                static_cast<Atom>(ev.xclient.data.l[0]) == wm_delete) {
                running = false;
                break;
            }

            if (ev.type == Expose && ev.xexpose.count == 0)
                app.needs_redraw = true;

            // ── Button press ──
            if (ev.type == ButtonPress) {
                int x = ev.xbutton.x, y = ev.xbutton.y;
                auto& sl  = app.slider_track;
                auto& btn = app.btn_capture;

                // Slider
                bool on_slider =
                    x >= sl.x && x <= sl.x + sl.w &&
                    y >= sl.y - 4 && y <= sl.y + sl.h + 4;
                if (on_slider) {
                    app.dragging_slider = true;
                    app.opacity = std::clamp(
                        (double)(x - sl.x) / sl.w, 0.05, 1.0);
                    apply_opacity(app);
                    app.needs_redraw = true;
                }

                // Capture button
                bool on_btn =
                    x >= btn.x && x <= btn.x + btn.w &&
                    y >= btn.y && y <= btn.y + btn.h;
                if (on_btn && !app.capturing && !app.waiting) {
                    app.capturing    = true;
                    app.needs_redraw = true;
                    draw(app);   // immediate feedback

                    std::thread([&app]() {
                        auto png = take_screenshot(app);
                        app.capturing = false;
                        app.waiting   = true;

                        // Poke the event loop
                        XExposeEvent xe{};
                        xe.type   = Expose;
                        xe.window = app.win;
                        xe.count  = 0;
                        XSendEvent(app.dpy, app.win, False,
                                   ExposureMask, reinterpret_cast<XEvent*>(&xe));
                        XFlush(app.dpy);

                        std::string result = png.empty()
                            ? "[Screenshot failed — no image data]"
                            : send_to_openrouter(png);

                        {
                            std::lock_guard<std::mutex> lk(app.response_mtx);
                            app.response_text = result;
                        }
                        app.waiting      = false;
                        app.needs_redraw = true;

                        XSendEvent(app.dpy, app.win, False,
                                   ExposureMask, reinterpret_cast<XEvent*>(&xe));
                        XFlush(app.dpy);
                    }).detach();
                }
            }

            // ── Button release ──
            if (ev.type == ButtonRelease)
                app.dragging_slider = false;

            // ── Mouse motion (slider drag) ──
            if (ev.type == MotionNotify && app.dragging_slider) {
                auto& sl = app.slider_track;
                app.opacity = std::clamp(
                    (double)(ev.xmotion.x - sl.x) / sl.w, 0.05, 1.0);
                apply_opacity(app);
                app.needs_redraw = true;
            }
        }

        if (app.needs_redraw) {
            app.needs_redraw = false;
            draw(app);
        }

        struct timespec ts = {0, 16'000'000};  // ~60 fps polling
        nanosleep(&ts, nullptr);
    }

    cairo_destroy(app.cr);
    cairo_surface_destroy(app.surf);
    XDestroyWindow(app.dpy, app.win);
    XCloseDisplay(app.dpy);
    curl_global_cleanup();
    return 0;
}
