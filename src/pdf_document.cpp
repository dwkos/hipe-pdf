#include "pdf_document.hpp"

#include <mupdf/fitz.h>

#include <cstring>
#include <stdexcept>

PdfDocument::PdfDocument(const std::string& path) : ctx(nullptr), doc(nullptr) {
	ctx = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
	if (!ctx)
		throw std::runtime_error("could not create MuPDF context");

	fz_register_document_handlers(ctx);

	fz_try(ctx) {
		doc = fz_open_document(ctx, path.c_str());
	} fz_catch(ctx) {
		std::string message = fz_caught_message(ctx);
		fz_drop_context(ctx);
		throw std::runtime_error("failed to open '" + path + "': " + message);
	}
}

PdfDocument::~PdfDocument() {
	if (doc) fz_drop_document(ctx, doc);
	if (ctx) fz_drop_context(ctx);
}

int PdfDocument::pageCount() const {
	int count = 0;
	fz_try(ctx) {
		count = fz_count_pages(ctx, doc);
	} fz_catch(ctx) {
		throw std::runtime_error(std::string("pageCount: ") + fz_caught_message(ctx));
	}
	return count;
}

void PdfDocument::pageSize(int pageNumber, float* widthPts, float* heightPts) const {
	fz_page* page = nullptr;
	fz_var(page);

	fz_try(ctx) {
		page = fz_load_page(ctx, doc, pageNumber);
		fz_rect bounds = fz_bound_page(ctx, page);
		if (widthPts) *widthPts = bounds.x1 - bounds.x0;
		if (heightPts) *heightPts = bounds.y1 - bounds.y0;
	} fz_always(ctx) {
		if (page) fz_drop_page(ctx, page);
	} fz_catch(ctx) {
		throw std::runtime_error("pageSize: page " + std::to_string(pageNumber) + ": " + fz_caught_message(ctx));
	}
}

std::vector<uint8_t> PdfDocument::renderPagePng(int pageNumber, float targetWidthPx) const {
	std::vector<uint8_t> result;

	fz_page* page = nullptr;
	fz_pixmap* pix = nullptr;
	fz_buffer* buf = nullptr;
	fz_output* out = nullptr;

	fz_var(page);
	fz_var(pix);
	fz_var(buf);
	fz_var(out);

	fz_try(ctx) {
		page = fz_load_page(ctx, doc, pageNumber);

		fz_rect bounds = fz_bound_page(ctx, page);
		float pageWidth = bounds.x1 - bounds.x0;
		float scale = (pageWidth > 0) ? (targetWidthPx / pageWidth) : 1.0f;
		fz_matrix ctm = fz_scale(scale, scale);

		pix = fz_new_pixmap_from_page(ctx, page, ctm, fz_device_rgb(ctx), 0);

		buf = fz_new_buffer(ctx, 4096);
		out = fz_new_output_with_buffer(ctx, buf);
		fz_write_pixmap_as_png(ctx, out, pix);
		fz_close_output(ctx, out);

		unsigned char* data;
		size_t size = fz_buffer_storage(ctx, buf, &data);
		result.assign(data, data + size);
	} fz_always(ctx) {
		if (out) fz_drop_output(ctx, out);
		if (buf) fz_drop_buffer(ctx, buf);
		if (pix) fz_drop_pixmap(ctx, pix);
		if (page) fz_drop_page(ctx, page);
	} fz_catch(ctx) {
		throw std::runtime_error("renderPagePng: page " + std::to_string(pageNumber) + ": " + fz_caught_message(ctx));
	}

	return result;
}

void PdfDocument::pageBackgroundColor(int pageNumber, uint8_t* r, uint8_t* g, uint8_t* b) const {
	const float SAMPLE_WIDTH_PX = 32.0f;

	int sumR = 255, sumG = 255, sumB = 255; /* fall back to white */

	fz_page* page = nullptr;
	fz_pixmap* pix = nullptr;
	fz_var(page);
	fz_var(pix);

	fz_try(ctx) {
		page = fz_load_page(ctx, doc, pageNumber);
		fz_rect bounds = fz_bound_page(ctx, page);
		float pageWidth = bounds.x1 - bounds.x0;
		float scale = (pageWidth > 0) ? (SAMPLE_WIDTH_PX / pageWidth) : 1.0f;
		pix = fz_new_pixmap_from_page(ctx, page, fz_scale(scale, scale), fz_device_rgb(ctx), 0);

		int w = fz_pixmap_width(ctx, pix);
		int h = fz_pixmap_height(ctx, pix);
		int n = fz_pixmap_components(ctx, pix);
		ptrdiff_t stride = fz_pixmap_stride(ctx, pix);
		unsigned char* samples = fz_pixmap_samples(ctx, pix);

		if (w > 0 && h > 0 && n >= 3) {
			int xs[2] = {0, w - 1};
			int ys[2] = {0, h - 1};
			sumR = sumG = sumB = 0;
			for (int yi = 0; yi < 2; yi++) {
				for (int xi = 0; xi < 2; xi++) {
					unsigned char* p = samples + ys[yi] * stride + xs[xi] * n;
					sumR += p[0]; sumG += p[1]; sumB += p[2];
				}
			}
			sumR /= 4; sumG /= 4; sumB /= 4;
		}
	} fz_always(ctx) {
		if (pix) fz_drop_pixmap(ctx, pix);
		if (page) fz_drop_page(ctx, page);
	} fz_catch(ctx) {
		/* keep the white fallback set above */
	}

	if (r) *r = (uint8_t) sumR;
	if (g) *g = (uint8_t) sumG;
	if (b) *b = (uint8_t) sumB;
}
