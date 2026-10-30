#include "settings/ocr_table.h"
#include <algorithm>
#include <cctype>
#include <vector>

namespace Vision
{
namespace
{
std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

void ReplaceAll(std::string &text, const std::string &from, const std::string &to)
{
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
        text.replace(at, from.size(), to);
}

// Cell text with its tags removed, entities decoded and line breaks folded, so one cell stays on one line.
std::string CellText(const std::string &html)
{
    std::string text;
    bool tag = false;
    for (char c : html)
    {
        if (c == '<')
            tag = true;
        else if (c == '>')
            tag = false;
        else if (!tag)
            text += c;
    }
    ReplaceAll(text, "&nbsp;", " ");
    ReplaceAll(text, "&lt;", "<");
    ReplaceAll(text, "&gt;", ">");
    ReplaceAll(text, "&quot;", "\"");
    ReplaceAll(text, "&amp;", "&");
    for (char &c : text)
        if (c == '\t' || c == '\r' || c == '\n')
            c = ' ';
    const auto first = text.find_first_not_of(' ');
    if (first == std::string::npos)
        return {};
    return text.substr(first, text.find_last_not_of(' ') - first + 1);
}

// Content of every <tag ...>...</tag> inside `html`, in order; tag names are matched case-insensitively.
std::vector<std::string> Elements(const std::string &html, const std::string &open, const std::string &alternative = {})
{
    std::vector<std::string> found;
    const auto lowered = Lower(html);
    size_t at = 0;
    while (true)
    {
        size_t start = lowered.find("<" + open, at);
        if (!alternative.empty())
        {
            const auto other = lowered.find("<" + alternative, at);
            if (other != std::string::npos && (start == std::string::npos || other < start))
                start = other;
        }
        if (start == std::string::npos)
            break;
        const auto name = lowered.compare(start, open.size() + 1, "<" + open) == 0 ? open : alternative;
        const auto gt = lowered.find('>', start);
        if (gt == std::string::npos)
            break;
        const auto close = lowered.find("</" + name + ">", gt);
        const auto end = close == std::string::npos ? lowered.size() : close;
        found.push_back(html.substr(gt + 1, end - gt - 1));
        at = close == std::string::npos ? lowered.size() : close + name.size() + 3;
    }
    return found;
}

std::string RenderTable(const std::string &table, const std::string &mode)
{
    std::vector<std::vector<std::string>> rows;
    for (const auto &row : Elements(table, "tr"))
    {
        std::vector<std::string> cells;
        for (const auto &cell : Elements(row, "td", "th"))
            cells.push_back(CellText(cell));
        if (!cells.empty())
            rows.push_back(std::move(cells));
    }
    size_t width = 0;
    for (const auto &row : rows)
        width = std::max(width, row.size());
    std::string out;
    for (size_t r = 0; r < rows.size(); ++r)
    {
        auto cells = rows[r];
        cells.resize(width);
        if (mode == "md")
        {
            out += "|";
            for (auto cell : cells)
            {
                ReplaceAll(cell, "|", "\\|");
                out += " " + cell + " |";
            }
            out += "\n";
            if (r == 0)
            {
                out += "|";
                for (size_t c = 0; c < width; ++c)
                    out += " --- |";
                out += "\n";
            }
        }
        else
        {
            for (size_t c = 0; c < cells.size(); ++c)
                out += (c ? "\t" : "") + cells[c];
            out += "\n";
        }
    }
    if (!out.empty() && out.back() == '\n')
        out.pop_back();
    return out;
}
} // namespace

std::string ConvertHtmlTables(const std::string &text, const std::string &mode)
{
    if (mode != "excel" && mode != "md")
        return text;
    const auto lowered = Lower(text);
    std::string out;
    size_t at = 0;
    while (true)
    {
        const auto start = lowered.find("<table", at);
        if (start == std::string::npos)
            break;
        const auto gt = lowered.find('>', start);
        if (gt == std::string::npos)
            break;
        const auto close = lowered.find("</table>", gt);
        const auto end = close == std::string::npos ? lowered.size() : close;
        out += text.substr(at, start - at);
        const auto rendered = RenderTable(text.substr(gt + 1, end - gt - 1), mode);
        out += rendered.empty() ? text.substr(start, (close == std::string::npos ? text.size() : close + 8) - start) : rendered;
        at = close == std::string::npos ? text.size() : close + 8;
    }
    out += text.substr(at);
    return out;
}
} // namespace Vision
