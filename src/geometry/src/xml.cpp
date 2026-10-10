// Потоковый разбор XML для 3MF.

#include "xml.hpp"

#include <algorithm>
#include <cstdint>

namespace kika::geometry::xml {

namespace {

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
bool is_name_char(char c) { return !is_space(c) && c != '=' && c != '>' && c != '/' && c != '"' && c != '\''; }

void append_utf8(std::string& out, std::uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

}  // namespace

std::string Reader::decode(std::string_view raw) {
  if (raw.find('&') == std::string_view::npos) return std::string(raw);
  std::string out;
  out.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] != '&') {
      out.push_back(raw[i]);
      continue;
    }
    const auto semi = raw.find(';', i);
    if (semi == std::string_view::npos) {
      out.push_back('&');
      continue;
    }
    const std::string_view ent = raw.substr(i + 1, semi - i - 1);
    if (ent == "amp")
      out.push_back('&');
    else if (ent == "lt")
      out.push_back('<');
    else if (ent == "gt")
      out.push_back('>');
    else if (ent == "quot")
      out.push_back('"');
    else if (ent == "apos")
      out.push_back('\'');
    else if (!ent.empty() && ent[0] == '#') {
      std::uint32_t cp = 0;
      const bool hex = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X');
      for (std::size_t k = hex ? 2 : 1; k < ent.size(); ++k) {
        const char c = ent[k];
        const int d = c >= '0' && c <= '9' ? c - '0'
                      : (hex && c >= 'a' && c <= 'f') ? c - 'a' + 10
                      : (hex && c >= 'A' && c <= 'F') ? c - 'A' + 10
                                                      : -1;
        if (d < 0) break;
        cp = cp * (hex ? 16u : 10u) + static_cast<std::uint32_t>(d);
      }
      append_utf8(out, cp);
    } else {
      out.append(raw.substr(i, semi - i + 1));  // неизвестная сущность — как есть
    }
    i = semi;
  }
  return out;
}

bool Reader::attr(std::string_view local_name, std::string& out) const {
  for (const auto& a : attrs_)
    if (local(a.name) == local_name) {
      out = decode(a.value);
      return true;
    }
  return false;
}

int Reader::line() const {
  return 1 + static_cast<int>(std::count(s_.begin(), s_.begin() + static_cast<std::ptrdiff_t>(std::min(p_, s_.size())), '\n'));
}

Reader::Event Reader::next() {
  if (pending_end_) {
    pending_end_ = false;
    return Event::End;
  }
  for (;;) {
    const auto lt = s_.find('<', p_);
    if (lt == std::string_view::npos) {
      p_ = s_.size();
      return Event::Eof;
    }
    p_ = lt + 1;
    if (p_ >= s_.size()) throw Error("XML обрывается");
    const char c = s_[p_];
    if (c == '?') {  // <?xml …?>
      const auto e = s_.find("?>", p_);
      if (e == std::string_view::npos) throw Error("XML: не закрыто «<?»");
      p_ = e + 2;
      continue;
    }
    if (c == '!') {
      if (s_.compare(p_, 3, "!--") == 0) {
        const auto e = s_.find("-->", p_ + 3);
        if (e == std::string_view::npos) throw Error("XML: не закрыт комментарий");
        p_ = e + 3;
      } else if (s_.compare(p_, 8, "![CDATA[") == 0) {
        const auto e = s_.find("]]>", p_ + 8);
        if (e == std::string_view::npos) throw Error("XML: не закрыт CDATA");
        p_ = e + 3;
      } else {  // <!DOCTYPE …>
        const auto e = s_.find('>', p_);
        if (e == std::string_view::npos) throw Error("XML обрывается");
        p_ = e + 1;
      }
      continue;
    }
    if (c == '/') {  // </name>
      ++p_;
      const auto b = p_;
      while (p_ < s_.size() && is_name_char(s_[p_])) ++p_;
      name_ = s_.substr(b, p_ - b);
      const auto e = s_.find('>', p_);
      if (e == std::string_view::npos) throw Error("XML обрывается");
      p_ = e + 1;
      attrs_.clear();
      return Event::End;
    }
    // <name a="1" b='2'> или <name/>
    const auto b = p_;
    while (p_ < s_.size() && is_name_char(s_[p_])) ++p_;
    name_ = s_.substr(b, p_ - b);
    attrs_.clear();
    for (;;) {
      while (p_ < s_.size() && is_space(s_[p_])) ++p_;
      if (p_ >= s_.size()) throw Error("XML обрывается");
      if (s_[p_] == '>') {
        ++p_;
        return Event::Start;
      }
      if (s_[p_] == '/') {
        if (p_ + 1 >= s_.size() || s_[p_ + 1] != '>') throw Error("XML: ошибка в строке " + std::to_string(line()));
        p_ += 2;
        pending_end_ = true;
        return Event::Start;
      }
      const auto nb = p_;
      while (p_ < s_.size() && is_name_char(s_[p_])) ++p_;
      const std::string_view an = s_.substr(nb, p_ - nb);
      while (p_ < s_.size() && is_space(s_[p_])) ++p_;
      if (p_ >= s_.size() || s_[p_] != '=') throw Error("XML: ошибка в атрибуте, строка " + std::to_string(line()));
      ++p_;
      while (p_ < s_.size() && is_space(s_[p_])) ++p_;
      if (p_ >= s_.size() || (s_[p_] != '"' && s_[p_] != '\'')) throw Error("XML: значение атрибута без кавычек, строка " + std::to_string(line()));
      const char q = s_[p_++];
      const auto ve = s_.find(q, p_);
      if (ve == std::string_view::npos) throw Error("XML обрывается");
      attrs_.push_back({an, s_.substr(p_, ve - p_)});
      p_ = ve + 1;
    }
  }
}

}  // namespace kika::geometry::xml
