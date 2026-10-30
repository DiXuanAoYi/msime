#pragma once
#include <string>

namespace Vision
{
// Local and cloud OCR models return tables as HTML. "excel" turns every <table> into tab-separated rows (pastes
// straight into Excel / WPS), "md" into a Markdown table, anything else leaves the text unchanged.
std::string ConvertHtmlTables(const std::string &text, const std::string &mode);
} // namespace Vision
