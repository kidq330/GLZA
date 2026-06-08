#pragma once

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace glza {

class GlzaError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class CompressError : public GlzaError {
 public:
  using GlzaError::GlzaError;
};

class EncodeError : public GlzaError {
 public:
  using GlzaError::GlzaError;
};

class DecodeError : public GlzaError {
 public:
  using GlzaError::GlzaError;
};

class FailDiagnostics {
  std::array<char, 24> stage_{};
  std::array<char, 768> detail_{};

 public:
  void clear() {
    stage_[0] = '\0';
    detail_[0] = '\0';
  }

  void set(const char* stage, const char* fmt, ...) {
    if (stage != nullptr) {
      std::strncpy(stage_.data(), stage, stage_.size() - 1);
      stage_[stage_.size() - 1] = '\0';
    }
    if (fmt == nullptr) return;
    std::va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(detail_.data(), detail_.size(), fmt, ap);
    va_end(ap);
  }

  [[nodiscard]] const char* last_stage() const { return stage_.data(); }
  [[nodiscard]] const char* last_detail() const { return detail_.data(); }
  [[nodiscard]] bool has_error() const { return stage_[0] != '\0'; }

  void capture_from(const GlzaError& e, const char* stage) {
    set(stage, "%s", e.what());
  }
};

inline FailDiagnostics& global_diagnostics() {
  static FailDiagnostics instance;
  return instance;
}

}  // namespace glza
