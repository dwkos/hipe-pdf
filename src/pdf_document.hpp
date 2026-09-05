#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct fz_context;
struct fz_document;

class PdfDocument {
	/* RAII wrapper around a MuPDF fz_context + fz_document pair. */
public:
	explicit PdfDocument(const std::string& path);
	/* Throws std::runtime_error if the file can't be opened or parsed. */

	~PdfDocument();

	PdfDocument(const PdfDocument&) = delete;
	PdfDocument& operator=(const PdfDocument&) = delete;

	int pageCount() const;

	void pageSize(int pageNumber, float* widthPts, float* heightPts) const;
	/* Native page size in PDF points (1/72 inch), without rasterizing it. Throws
	 * std::runtime_error on failure. */

	std::vector<uint8_t> renderPagePng(int pageNumber, float targetWidthPx) const;
	/* Rasterizes the given 0-indexed page to a PNG image scaled so its width matches
	 * targetWidthPx (height follows the page's aspect ratio). Throws std::runtime_error
	 * on failure, including if the render takes too long (a pathologically complex page is
	 * aborted after a fixed timeout rather than hanging indefinitely). */

	void pageBackgroundColor(int pageNumber, uint8_t* r, uint8_t* g, uint8_t* b) const;
	/* Cheap heuristic for chrome-matching purposes (e.g. a slideshow surround): renders
	 * the page very small and averages its four corner pixels, on the assumption that
	 * page corners are usually background rather than content. Not real background
	 * detection. Falls back to white on failure rather than throwing. */

	struct TextSpan {
		std::string text;
		float x, y, width, height; /* PDF points, same page space renderPagePng scales from */
	};

	std::vector<TextSpan> pageTextSpans(int pageNumber) const;
	/* One span per line of extracted text, for building a selectable text overlay atop
	 * the raster render (positions/sizes are in PDF points -- the caller scales them by
	 * the same factor used for the raster width). Throws std::runtime_error on failure. */

private:
	fz_context* ctx;
	fz_document* doc;
};
