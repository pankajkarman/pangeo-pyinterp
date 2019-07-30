// Copyright (c) 2019 CNES
//
// All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.
#pragma once

namespace pyinterp::detail::math {

/// Linear interpolation
///
/// @param x x_coordinate
/// @param x0 x0 coordinate
/// @param x1 x1 coordinate
/// @param y0 Point value for the coordinate (x0)
/// @param y1 Point value for the coordinate (x1)
template <typename T>
inline constexpr T linear(const T& x, const T& x0, const T& x1, const T& y0,
                          const T& y1) {
  auto dx = (x1 - x0);
  auto t = (x1 - x) / dx;
  auto u = (x - x0) / dx;

  return t * y0 + u * y1 / (t + u);
}

}  // namespace pyinterp::detail::math
