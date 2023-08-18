#include "phodispl/infobar.hpp"
#include "phodispl/config.hpp"
#include "phodispl/fade-widget.hpp"
#include "phodispl/fonts.hpp"
#include "phodispl/image.hpp"
#include "resources.hpp"

#include <codecvt>
#include <locale>
#include <string>
#include <string_view>

#include <gl/primitives.hpp>

#include <pixglot/codecs.hpp>
#include <pixglot/frame.hpp>
#include <pixglot/frame-source-info.hpp>

#include <win/viewport.hpp>



namespace {
  template<typename T>
  [[nodiscard]] bool assign_diff(T& target, T source) {
    if (source != target) {
      target = std::move(source);
      return true;
    }
    return false;
  }



  [[nodiscard]] std::u32string convert_string(std::string_view str) {
    return std::wstring_convert<std::codecvt_utf8<char32_t>, char32_t>{}
              .from_bytes(str.begin(), str.end());
  }



  [[nodiscard]] std::u32string_view stringify(pixglot::color_model model) {
    using enum pixglot::color_model;
    switch (model) {
      case yuv:     return U"yuv";
      case rgb:     return U"RGB";
      case palette: return U"Palette";
      case value:   return U"Value";
      case unknown: break;
    }
    return U"Unknown";
  }



  [[nodiscard]] std::u32string_view stringify(pixglot::codec codec) {
    using enum pixglot::codec;
    switch (codec) {
      case avif: return U"Avif";
      case jpeg: return U"Jpeg";
      case png:  return U"PNG";
      case exr:  return U"EXR";
      case ppm:  return U"PPM";
      case webp: return U"WebP";
      case gif:  return U"Gif";
      case jxl:  return U"Jxl";
    }
    return U"Unknown";
  }



  [[nodiscard]] std::u32string format_fsi(const pixglot::frame_source_info& fsi) {
    std::u32string output;

    output += ::stringify(fsi.color_model());

    if (fsi.color_model() == pixglot::color_model::yuv) {
      output += convert_string(std::to_string(std::to_underlying(fsi.subsampling())));
    }

    if (fsi.has_alpha()) {
      output.push_back(U'A');
    }

    output.push_back(U' ');

    auto format = fsi.color_model_format();
    output += convert_string(pixglot::stringify(format[0]));

    bool expand = (fsi.has_color() && (format[0] != format[1] || format[0] != format[2]))
                  || (fsi.has_alpha() && (format[0] != format[3]));

    if (expand) {
      if (fsi.has_color()) {
        output += U", ";
        output += convert_string(pixglot::stringify(format[1]));
        output += U", ";
        output += convert_string(pixglot::stringify(format[2]));
      }

      if (fsi.has_alpha()) {
        output += U", ";
        output += convert_string(pixglot::stringify(format[3]));
      }
    }


    return output;
  }



  [[nodiscard]] std::u32string format_size(size_t width, size_t height) {
    return convert_string(std::to_string(width)) + U'×'
      + convert_string(std::to_string(height));
  }
}





void infobar::set_frame(const pixglot::frame_view& frame) {
  invalidate(assign_diff(str_format_, format_fsi (frame.source_info())));
  invalidate(assign_diff(str_size_,   format_size(frame.width(), frame.height())));
}



void infobar::clear_frame() {
  invalidate(assign_diff<std::u32string>(str_format_, U""));
  invalidate(assign_diff<std::u32string>(str_size_,   U""));
}





void infobar::set_image(const image& img) {
  invalidate(assign_diff(str_path_, img.path().parent_path().u32string()));
  invalidate(assign_diff(str_name_, img.path().filename().u32string()));

  if (img.loading() || img.finished()) {
    invalidate(assign_diff(codec_, img.codec()));
  } else {
    invalidate(assign_diff(codec_, {}));
  }
}



void infobar::clear_image() {
  invalidate(assign_diff<std::u32string>(str_path_, U""));
  invalidate(assign_diff<std::u32string>(str_name_, U""));
  invalidate(assign_diff(codec_, {}));
}





infobar::infobar() :
  quad_  {gl::primitives::quad()},
  shader_{
    resources::shader_plane_object_vs_sv(),
    resources::shader_plane_solid_fs_sv()
  },
  shader_trafo_{shader_.uniform("transform")},
  shader_color_{shader_.uniform("color")}
{}





void infobar::on_pointer_enter(win::vec2<float> /*pos*/) {
  mouse_leave_ = std::chrono::steady_clock::now();

  fade_widget::show();

  mouse_inside_ = true;
}



void infobar::on_pointer_leave() {
  mouse_inside_ = false;
  mouse_leave_ = std::chrono::steady_clock::now();
}





void infobar::on_update() {
  if (!mouse_inside_ &&
      std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - mouse_leave_).count() > 2) {
    fade_widget::hide();
  }
}





namespace {
  [[nodiscard]] color premultiply(color c, float alpha) {
    return {c[0] * c[3] * alpha, c[1] * c[3] * alpha, c[2] * c[3] * alpha, c[3] * alpha};
  }



  void print(
      std::u32string_view  key,
      std::u32string_view  value,
      win::vec2<float>&    position,
      const win::viewport& viewport,
      float                alpha
  ) {
    viewport.draw_string(position, key, font_main, global_config().theme_text_size,
        premultiply(global_config().theme_text_color, alpha));

    auto pos = position + win::vec2<float>{global_config().theme_text_size * 6.f, 0.f};

    position.y += viewport.draw_string(pos, value,
        font_main, global_config().theme_text_size,
        premultiply(global_config().theme_text_color, alpha)).y;

    position.y += global_config().theme_text_size * 1.25f;
  }
}





void infobar::on_render() {
  if (!visible()) {
    return;
  }

  shader_.use();
  glUniform4f(shader_color_, 0.f, 0.f, 0.f, 0.7f * opacity());

  win::set_uniform_mat4(shader_trafo_,
      trafo_mat_logical({0.f, 0.f}, logical_size()));

  quad_.draw();

  auto start = logical_position() + static_cast<float>(global_config().theme_text_size)
                                      * win::vec2<float>{1.0f, 1.5f};

  auto offset = start;

  offset += viewport().draw_string(offset, str_name_,
      font_main, global_config().theme_text_size,
      premultiply(global_config().theme_text_color, opacity()));

  offset.x += global_config().theme_text_size * 1.f;

  offset.y += viewport().draw_string(offset, str_path_,
      font_main, global_config().theme_text_size,
      premultiply(global_config().theme_text_color, 0.75f * opacity())).y;

  offset.y += global_config().theme_text_size * 1.5f;
  offset.x = start.x;


  if (codec_) {
    print(U"Format:",       ::stringify(*codec_), offset, viewport(), opacity());
    print(U"Pixel Format:", str_format_,          offset, viewport(), opacity());
    print(U"Size:",         str_size_,            offset, viewport(), opacity());
  }
}