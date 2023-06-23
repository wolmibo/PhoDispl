// Copyright (c) 2023 wolmibo
// SPDX-License-Identifier: MIT

#ifndef PHODISPL_IMAGE_DISPLAY_HPP_INCLUDED
#define PHODISPL_IMAGE_DISPLAY_HPP_INCLUDED

#include "phodispl/animation.hpp"
#include "phodispl/config-types.hpp"
#include "phodispl/image.hpp"

#include <gl/mesh.hpp>
#include <gl/program.hpp>

#include <win/widget.hpp>



enum class dynamic_scale {
  fit,
  clip,
};

using scale_mode = std::variant<dynamic_scale, float>;



class image_display : public win::widget {
  public:
    image_display();

    void active(std::shared_ptr<image>);

    void exposure(float);
    void exposure_multiply(float);

    void scale_filter_toggle();

    void scale(scale_mode);
    void scale_multiply_at(float, win::vec2<float>);



  private:
    std::shared_ptr<image> current_;
    std::shared_ptr<image> previous_;

    animation_time         crossfade_;

    gl::mesh               quad_;
    gl::program            crossfade_shader_;
    GLint                  crossfade_shader_factor_a_;
    GLint                  crossfade_shader_factor_b_;

    animation<float>       exposure_;
    scale_filter           scale_filter_;



    void on_update() override;
    void on_render() override;
};

#endif // PHODISPL_IMAGE_DISPLAY_HPP_INCLUDED
