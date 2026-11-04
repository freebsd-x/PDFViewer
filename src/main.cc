// PDFViewer.xapp — PDF document viewer for FreeBSD-X
//
// Layout:
//   Top toolbar:  Open, ◀ page X / N ▶, zoom strip, search bar + ▲▼ + match count
//   Main area:    ScrollViewWidget — rendered page as ImageWidget (pixel data)
//
// Features:
//   • Poppler-cpp page rendering at configurable DPI (72 * zoom)
//   • Zoom: 50 / 75 / 100 / 125 / 150 / 200 %
//   • Page navigation: prev/next buttons
//   • Text search: scan all pages, report "match X of N" and jump to matching page
//   • ARGB32→RGBA pixel conversion (poppler little-endian BGRA → OpenGL RGBA)
//   • Opens via --open <path> (before Capsicum sandbox), positional arg, or Open…

#include "xkit/widget.h"
#include "xkit/application.h"
#include "xkit/xbus_client.h"
#include "xkit/file_dialog.h"
#include "xkit/media_file.h"
#include "shared/protocol.h"
#include <poppler/cpp/poppler-document.h>
#include <poppler/cpp/poppler-page.h>
#include <poppler/cpp/poppler-page-renderer.h>
#include <poppler/cpp/poppler-image.h>
#include <memory>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <algorithm>

using namespace fbx::xkit;
using namespace fbx::xrender;
using namespace fbx::shared;

// ── Zoom table ────────────────────────────────────────────────────────────────

static const float kZoomLevels[] = {0.50f, 0.75f, 1.00f, 1.25f, 1.50f, 2.00f};
static constexpr int kZoomCount  = 6;
static constexpr int kZoomDefault = 2; // 1.00×

// ── Pixel format conversion: poppler ARGB32 (LE=BGRA) → xrender RGBA ─────────

static std::vector<uint8_t> bgraToRgba(const char* src, int w, int h, int bpr) {
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = reinterpret_cast<const uint8_t*>(src + y * bpr);
        uint8_t*       dst = out.data() + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
            dst[x*4+0] = row[x*4+2];
            dst[x*4+1] = row[x*4+1];
            dst[x*4+2] = row[x*4+0];
            dst[x*4+3] = row[x*4+3];
        }
    }
    return out;
}

static std::string basename(const std::string& path) {
    auto p = path.rfind('/');
    return (p == std::string::npos) ? path : path.substr(p + 1);
}

// ── App ───────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    std::string open_path;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--open") {
            open_path = argv[i + 1];
            break;
        }
    }
    if (open_path.empty() && argc == 2 && argv[1][0] != '-')
        open_path = argv[1];

    // Load document before XApplication::Run (which calls cap_enter).
    auto doc = std::make_shared<std::unique_ptr<poppler::document>>();
    int n_pages = 0;
    if (!open_path.empty()) {
        doc->reset(poppler::document::load_from_file(open_path));
        if (*doc) n_pages = (*doc)->pages();
    }

    // ── Shared state ─────────────────────────────────────────────────────────
    auto current_page  = std::make_shared<int>(0);
    auto zoom_idx      = std::make_shared<int>(kZoomDefault);
    auto doc_ptr       = std::make_shared<poppler::document*>(doc->get());
    auto total_pages   = std::make_shared<int>(n_pages);

    struct SearchMatch { int page; };
    auto search_query   = std::make_shared<std::string>("");
    auto search_matches = std::make_shared<std::vector<SearchMatch>>();
    auto search_cur_idx = std::make_shared<int>(0);

    // Lambdas — forward-declared
    auto render_page = std::make_shared<std::function<void()>>();
    auto run_search  = std::make_shared<std::function<void()>>();

    // ── Widgets ───────────────────────────────────────────────────────────────
    auto fname_lbl = std::make_shared<TextWidget>(
        open_path.empty() ? "No document" : basename(open_path), 12.0f);
    fname_lbl->setColor(Color{0.85f, 0.85f, 0.85f, 1.0f});
    fname_lbl->setFixedSize(140.0f, 0.0f);

    // Page nav
    auto page_num_lbl = std::make_shared<TextWidget>("— / —", 12.0f);
    page_num_lbl->setColor(Color{0.85f, 0.85f, 0.85f, 1.0f});
    page_num_lbl->setFixedSize(70.0f, 0.0f);

    auto prev_btn = std::make_shared<ButtonWidget>("◀",
        [current_page, total_pages, render_page]() {
            if (*current_page > 0) {
                --(*current_page);
                if (*render_page) (*render_page)();
            }
        });
    prev_btn->setFixedSize(28.0f, 28.0f);
    prev_btn->setBackgroundColor(Color{0.22f, 0.22f, 0.22f, 1.0f});

    auto next_btn = std::make_shared<ButtonWidget>("▶",
        [current_page, total_pages, render_page]() {
            if (*current_page + 1 < *total_pages) {
                ++(*current_page);
                if (*render_page) (*render_page)();
            }
        });
    next_btn->setFixedSize(28.0f, 28.0f);
    next_btn->setBackgroundColor(Color{0.22f, 0.22f, 0.22f, 1.0f});

    // Zoom
    char zoom_init[8];
    std::snprintf(zoom_init, sizeof(zoom_init), "%d%%",
                  static_cast<int>(kZoomLevels[kZoomDefault] * 100));
    auto zoom_lbl = std::make_shared<TextWidget>(zoom_init, 12.0f);
    zoom_lbl->setColor(Color{0.85f, 0.85f, 0.85f, 1.0f});
    zoom_lbl->setFixedSize(40.0f, 0.0f);

    auto zoom_out_btn = std::make_shared<ButtonWidget>("−",
        [zoom_idx, zoom_lbl, render_page]() {
            if (*zoom_idx > 0) {
                --(*zoom_idx);
                char buf[8];
                std::snprintf(buf, sizeof(buf), "%d%%",
                    static_cast<int>(kZoomLevels[*zoom_idx] * 100));
                zoom_lbl->setText(buf);
                if (*render_page) (*render_page)();
            }
        });
    zoom_out_btn->setFixedSize(26.0f, 28.0f);
    zoom_out_btn->setBackgroundColor(Color{0.22f, 0.22f, 0.22f, 1.0f});

    auto zoom_in_btn = std::make_shared<ButtonWidget>("+",
        [zoom_idx, zoom_lbl, render_page]() {
            if (*zoom_idx < kZoomCount - 1) {
                ++(*zoom_idx);
                char buf[8];
                std::snprintf(buf, sizeof(buf), "%d%%",
                    static_cast<int>(kZoomLevels[*zoom_idx] * 100));
                zoom_lbl->setText(buf);
                if (*render_page) (*render_page)();
            }
        });
    zoom_in_btn->setFixedSize(26.0f, 28.0f);
    zoom_in_btn->setBackgroundColor(Color{0.22f, 0.22f, 0.22f, 1.0f});

    // Search
    auto search_input = std::make_shared<TextInputWidget>("");
    search_input->setPlaceholder("Search text…");
    search_input->setFixedSize(160.0f, 28.0f);
    search_input->setOnChanged([search_query, search_matches, search_cur_idx,
                                run_search](const std::string& q) {
        *search_query   = q;
        search_matches->clear();
        *search_cur_idx = 0;
        if (!q.empty() && *run_search) (*run_search)();
    });

    auto match_lbl = std::make_shared<TextWidget>("", 11.0f);
    match_lbl->setColor(Color{0.65f, 0.65f, 0.65f, 1.0f});
    match_lbl->setFixedSize(80.0f, 0.0f);

    auto match_prev_btn = std::make_shared<ButtonWidget>("▲",
        [search_cur_idx, search_matches, current_page, render_page, match_lbl]() {
            if (search_matches->empty()) return;
            int n = static_cast<int>(search_matches->size());
            *search_cur_idx = (*search_cur_idx - 1 + n) % n;
            *current_page   = (*search_matches)[static_cast<size_t>(*search_cur_idx)].page;
            char buf[24];
            std::snprintf(buf, sizeof(buf), "%d / %d", *search_cur_idx + 1, n);
            match_lbl->setText(buf);
            if (*render_page) (*render_page)();
        });
    match_prev_btn->setFixedSize(24.0f, 28.0f);
    match_prev_btn->setBackgroundColor(Color{0.22f, 0.22f, 0.22f, 1.0f});

    auto match_next_btn = std::make_shared<ButtonWidget>("▼",
        [search_cur_idx, search_matches, current_page, render_page, match_lbl]() {
            if (search_matches->empty()) return;
            int n = static_cast<int>(search_matches->size());
            *search_cur_idx = (*search_cur_idx + 1) % n;
            *current_page   = (*search_matches)[static_cast<size_t>(*search_cur_idx)].page;
            char buf[24];
            std::snprintf(buf, sizeof(buf), "%d / %d", *search_cur_idx + 1, n);
            match_lbl->setText(buf);
            if (*render_page) (*render_page)();
        });
    match_next_btn->setFixedSize(24.0f, 28.0f);
    match_next_btn->setBackgroundColor(Color{0.22f, 0.22f, 0.22f, 1.0f});

    // ── Toolbar ───────────────────────────────────────────────────────────────
    auto div_w = [](float w) {
        auto d = std::make_shared<FlexRow>();
        d->setFixedSize(w, 28.0f);
        d->setBackgroundColor(Color{0.28f, 0.28f, 0.28f, 1.0f});
        return d;
    };

    // Past start-up the sandbox opens no paths: the user picks, in the shell.
    auto open_btn = std::make_shared<ButtonWidget>("Open…",
        [=]() {
            auto* bus = XApplication::getBus();
            if (!bus) return;
            XFileDialog::pickToOpen(*bus, "Open PDF", "", [=](XFileDialog::Picked p) {
                if (p.cancelled) return;
                if (!p.fd) { fname_lbl->setText("Couldn't open that file: " + p.error); return; }
                std::string bytes;
                if (!readAllFd(p.fd.get(), bytes)) { fname_lbl->setText("Couldn't read that file"); return; }
                poppler::byte_array data(bytes.begin(), bytes.end());
                std::unique_ptr<poppler::document> opened(poppler::document::load_from_data(&data));
                if (!opened || opened->is_locked()) {
                    fname_lbl->setText(basename(p.path) + (opened ? " is locked" : " isn't a readable PDF"));
                    return;
                }
                // (load_from_data takes over `data`; a bad file left the open one alone)
                *doc = std::move(opened);
                *doc_ptr = doc->get();
                *total_pages = (*doc)->pages();
                *current_page = 0;
                fname_lbl->setText(basename(p.path));
                XApplication::setTitle("PDF Viewer — " + basename(p.path));
                if (*render_page) (*render_page)();
            });
        });

    auto toolbar = std::make_shared<FlexRow>();
    toolbar->setFixedSize(0.0f, 44.0f);
    toolbar->setPadding(6.0f);
    toolbar->setGap(6.0f);
    toolbar->setBackgroundColor(Color{0.14f, 0.14f, 0.14f, 1.0f});
    toolbar->setCrossAxisAlignment(CrossAxisAlignment::Center);
    toolbar->addChild(open_btn);
    toolbar->addChild(fname_lbl);
    toolbar->addChild(div_w(1.0f));
    toolbar->addChild(prev_btn);
    toolbar->addChild(page_num_lbl);
    toolbar->addChild(next_btn);
    toolbar->addChild(div_w(1.0f));
    toolbar->addChild(zoom_out_btn);
    toolbar->addChild(zoom_lbl);
    toolbar->addChild(zoom_in_btn);
    toolbar->addChild(div_w(1.0f));
    toolbar->addChild(search_input);
    toolbar->addChild(match_prev_btn);
    toolbar->addChild(match_next_btn);
    toolbar->addChild(match_lbl);

    // ── Page content area ─────────────────────────────────────────────────────
    auto page_content = std::make_shared<FlexColumn>();
    page_content->setCrossAxisAlignment(CrossAxisAlignment::Center);
    page_content->setPadding(20.0f);
    page_content->setBackgroundColor(Color{0.22f, 0.22f, 0.22f, 1.0f});

    auto scroll = std::make_shared<ScrollViewWidget>(page_content);
    scroll->setFlexGrow(1.0f);

    // ── Root ──────────────────────────────────────────────────────────────────
    auto root_col = std::make_shared<FlexColumn>();
    root_col->setBackgroundColor(Color{0.10f, 0.10f, 0.10f, 1.0f});
    root_col->addChild(toolbar);
    root_col->addChild(std::make_shared<DividerWidget>(1.0f));
    root_col->addChild(scroll);

    auto root = std::make_shared<ContainerWidget>(root_col);

    return XApplication::Run(root, "/tmp/freebsdx.sock", false, false,
        [=](XBusClient& xbus) {
            XApplication::setTitle("PDF Viewer");

            // ── render_page ───────────────────────────────────────────────────
            *render_page = [=]() {
                page_content->clearChildren();

                // Update nav label
                if (*total_pages > 0) {
                    std::string s = std::to_string(*current_page + 1)
                                  + " / " + std::to_string(*total_pages);
                    page_num_lbl->setText(s);
                } else {
                    page_num_lbl->setText("— / —");
                }

                poppler::document* d = *doc_ptr;
                if (!d || *total_pages == 0) {
                    auto lbl = std::make_shared<TextWidget>(
                        "No PDF open. Choose Open… to pick one.", 14.0f);
                    lbl->setColor(Color{0.60f, 0.60f, 0.60f, 1.0f});
                    lbl->setPadding(40.0f);
                    page_content->addChild(lbl);
                    return;
                }

                int pg   = std::clamp(*current_page, 0, *total_pages - 1);
                float dpi = 96.0f * kZoomLevels[*zoom_idx];

                std::unique_ptr<poppler::page> page(d->create_page(pg));
                if (!page) return;

                poppler::page_renderer renderer;
                renderer.set_render_hint(
                    static_cast<poppler::page_renderer::render_hint>(
                        poppler::page_renderer::text_antialiasing |
                        poppler::page_renderer::antialiasing), true);

                poppler::image img = renderer.render_page(page.get(), dpi, dpi);
                if (!img.is_valid()) return;

                int pw = img.width();
                int ph = img.height();
                std::vector<uint8_t> rgba = bgraToRgba(img.data(), pw, ph,
                                                       img.bytes_per_row());

                auto img_widget = std::make_shared<ImageWidget>(
                    std::move(rgba),
                    static_cast<uint32_t>(pw),
                    static_cast<uint32_t>(ph));
                img_widget->setFixedSize(static_cast<float>(pw),
                                        static_cast<float>(ph));
                page_content->addChild(img_widget);
            };

            // ── run_search ────────────────────────────────────────────────────
            *run_search = [=]() {
                search_matches->clear();
                *search_cur_idx = 0;

                poppler::document* d = *doc_ptr;
                if (!d || search_query->empty()) {
                    match_lbl->setText("");
                    return;
                }

                poppler::ustring query = poppler::ustring::from_utf8(
                    search_query->c_str(),
                    static_cast<int>(search_query->size()));

                for (int i = 0; i < *total_pages; ++i) {
                    std::unique_ptr<poppler::page> pg(d->create_page(i));
                    if (!pg) continue;
                    poppler::rectf dummy;
                    bool found = pg->search(query, dummy,
                                            poppler::page::search_from_top,
                                            poppler::case_insensitive);
                    if (found) search_matches->push_back({i});
                }

                if (!search_matches->empty()) {
                    *current_page   = (*search_matches)[0].page;
                    *search_cur_idx = 0;
                    char buf[24];
                    std::snprintf(buf, sizeof(buf), "1 / %zu",
                                  search_matches->size());
                    match_lbl->setText(buf);
                    if (*render_page) (*render_page)();
                } else {
                    match_lbl->setText("No matches");
                }
            };

            // ── Initial render ────────────────────────────────────────────────
            if (*render_page) (*render_page)();
        });
}
