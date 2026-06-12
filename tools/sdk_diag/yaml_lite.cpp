#include "yaml_lite.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace sdk_diag {
namespace {

struct Line {
  int indent;
  std::string content;  // leading indent removed, comments stripped, right-trimmed
  int lineno;           // 1-based, for error messages
};

std::string RTrim(const std::string& s) {
  std::size_t e = s.size();
  while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
  return s.substr(0, e);
}

std::string Trim(const std::string& s) {
  std::size_t b = 0;
  while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r')) ++b;
  std::size_t e = s.size();
  while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
  return s.substr(b, e - b);
}

// Strip an unquoted trailing/whole-line comment, respecting quotes.
std::string StripComment(const std::string& s) {
  bool in_single = false, in_double = false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == '\'' && !in_double) {
      in_single = !in_single;
    } else if (c == '"' && !in_single) {
      in_double = !in_double;
    } else if (c == '#' && !in_single && !in_double) {
      if (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t') {
        return s.substr(0, i);
      }
    }
  }
  return s;
}

std::string Unquote(const std::string& in) {
  std::string s = Trim(in);
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
    std::string out;
    for (std::size_t i = 1; i + 1 < s.size(); ++i) {
      if (s[i] == '\\' && i + 2 < s.size()) {
        char n = s[i + 1];
        if (n == 'n') {
          out += '\n';
          ++i;
          continue;
        }
        if (n == 't') {
          out += '\t';
          ++i;
          continue;
        }
        if (n == '"' || n == '\\') {
          out += n;
          ++i;
          continue;
        }
      }
      out += s[i];
    }
    return out;
  }
  if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') {
    std::string out;
    for (std::size_t i = 1; i + 1 < s.size(); ++i) {
      if (s[i] == '\'' && i + 1 < s.size() - 1 && s[i + 1] == '\'') {
        out += '\'';
        ++i;
        continue;
      }
      out += s[i];
    }
    return out;
  }
  return s;
}

bool IsSeqEntry(const Line& l) {
  return l.content == "-" || (l.content.size() >= 2 && l.content[0] == '-' && l.content[1] == ' ');
}

// True when a sequence item's inline content begins a mapping ("key: ...").
bool IsMapEntryStart(const std::string& s) {
  if (s.empty()) return false;
  if (s[0] == '"' || s[0] == '\'') return false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == ':') {
      bool sep = (i + 1 == s.size()) || s[i + 1] == ' ';
      return sep;  // key part (s[0:i]) is single token by schema convention
    }
    if (c == ' ') return false;  // space before any colon -> not a "key:" form
  }
  return false;
}

class Parser {
 public:
  explicit Parser(std::vector<Line> lines) : lines_(std::move(lines)) {}

  bool Parse(YamlNode& out, std::string& err) {
    if (lines_.empty()) {
      out.type = YamlNode::Type::Null;
      return true;
    }
    if (lines_[0].indent != 0) {
      err = Fail(lines_[0].lineno, "top-level content must not be indented");
      return false;
    }
    std::size_t idx = 0;
    out = ParseBlock(lines_, idx, err);
    if (!err.empty()) return false;
    if (idx < lines_.size()) {
      err = Fail(lines_[idx].lineno, "unexpected indentation");
      return false;
    }
    return true;
  }

 private:
  static std::string Fail(int lineno, const std::string& msg) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "line %d: ", lineno);
    return std::string(buf) + msg;
  }

  YamlNode ParseBlock(const std::vector<Line>& lines, std::size_t& idx, std::string& err) {
    YamlNode node;
    if (idx >= lines.size()) return node;
    int first_indent = lines[idx].indent;

    if (IsSeqEntry(lines[idx])) {
      node.type = YamlNode::Type::Sequence;
      while (idx < lines.size() && lines[idx].indent == first_indent && IsSeqEntry(lines[idx]) &&
             err.empty()) {
        const std::string& content = lines[idx].content;
        std::string after = (content == "-") ? "" : Trim(content.substr(1));
        int item_lineno = lines[idx].lineno;
        ++idx;

        if (after.empty()) {
          if (idx < lines.size() && lines[idx].indent > first_indent) {
            node.seq.push_back(ParseBlock(lines, idx, err));
          } else {
            YamlNode null_node;
            node.seq.push_back(null_node);
          }
        } else if (IsMapEntryStart(after)) {
          int cont_base = first_indent + 2;
          std::vector<Line> sub;
          sub.push_back({0, after, item_lineno});
          while (idx < lines.size() && lines[idx].indent > first_indent) {
            int ni = lines[idx].indent - cont_base;
            if (ni < 0) ni = 0;
            sub.push_back({ni, lines[idx].content, lines[idx].lineno});
            ++idx;
          }
          std::size_t sub_idx = 0;
          node.seq.push_back(ParseBlock(sub, sub_idx, err));
        } else {
          YamlNode s;
          s.type = YamlNode::Type::Scalar;
          s.scalar = Unquote(after);
          node.seq.push_back(s);
        }
      }
      return node;
    }

    node.type = YamlNode::Type::Map;
    while (idx < lines.size() && lines[idx].indent == first_indent && !IsSeqEntry(lines[idx]) &&
           err.empty()) {
      const std::string& content = lines[idx].content;
      std::size_t colon = content.find(':');
      if (colon == std::string::npos) {
        err = Fail(lines[idx].lineno, "expected 'key: value'");
        return node;
      }
      std::string key = Unquote(Trim(content.substr(0, colon)));
      std::string rest = Trim(content.substr(colon + 1));
      int key_indent = first_indent;
      ++idx;

      YamlNode child;
      if (!rest.empty()) {
        child.type = YamlNode::Type::Scalar;
        child.scalar = Unquote(rest);
      } else if (idx < lines.size() && lines[idx].indent > key_indent) {
        child = ParseBlock(lines, idx, err);
      }
      node.map.emplace_back(std::move(key), std::move(child));
    }
    return node;
  }

  std::vector<Line> lines_;
};

}  // namespace

const YamlNode* YamlNode::Find(const std::string& key) const {
  if (type != Type::Map) return nullptr;
  for (const auto& kv : map) {
    if (kv.first == key) return &kv.second;
  }
  return nullptr;
}

const std::string& YamlNode::Str() const {
  static const std::string kEmpty;
  return type == Type::Scalar ? scalar : kEmpty;
}

YamlParseResult ParseYaml(const std::string& text) {
  YamlParseResult result;

  std::vector<Line> lines;
  std::istringstream iss(text);
  std::string raw;
  int lineno = 0;
  while (std::getline(iss, raw)) {
    ++lineno;
    // Count leading indent; reject tabs in indentation.
    int indent = 0;
    std::size_t i = 0;
    bool tab_in_indent = false;
    while (i < raw.size() && (raw[i] == ' ' || raw[i] == '\t')) {
      if (raw[i] == '\t') tab_in_indent = true;
      ++indent;
      ++i;
    }
    std::string content = StripComment(raw.substr(i));
    content = RTrim(content);
    if (content.empty()) continue;          // blank or comment-only
    if (content == "---" || content == "...") continue;  // document markers
    if (tab_in_indent) {
      result.error = "line " + std::to_string(lineno) + ": tab character in indentation";
      return result;
    }
    lines.push_back({indent, content, lineno});
  }

  Parser parser(std::move(lines));
  std::string err;
  if (!parser.Parse(result.root, err)) {
    result.error = err;
    return result;
  }
  result.ok = true;
  return result;
}

YamlParseResult ParseYamlFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    YamlParseResult r;
    r.error = "cannot open file: " + path;
    return r;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ParseYaml(ss.str());
}

}  // namespace sdk_diag
