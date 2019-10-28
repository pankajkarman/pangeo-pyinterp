// Copyright (c) 2019 CNES
//
// All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.
#pragma once
#include <pybind11/eigen.h>
#include <pybind11/numpy.h>
#include <pybind11/stl.h>
#include <Eigen/Core>
#include <atomic>
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/mean.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include "pyinterp/detail/math.hpp"
#include "pyinterp/detail/thread.hpp"
#include "pyinterp/grid.hpp"

namespace pyinterp {
namespace detail {

/// Calculate the zonal average in x direction
///
/// @param grid The grid to be processed.
/// @param mask Matrix describing the undefined pixels of the grid providedNaN
/// NumberReplaces all missing (_FillValue) values in a grid with values derived
/// from solving Poisson's equation via relaxation. of threads used for the
/// calculation
///
/// @param grid
template <typename Type>
void set_zonal_average(
    pybind11::EigenDRef<Eigen::Matrix<Type, Eigen::Dynamic, Eigen::Dynamic>>&
        grid,
    Eigen::Matrix<bool, Eigen::Dynamic, Eigen::Dynamic>& mask,
    const size_t num_threads) {
  // Captures the detected exceptions in the calculation function
  // (only the last exception captured is kept)
  auto except = std::exception_ptr(nullptr);

  detail::dispatch(
      [&](size_t y_start, size_t y_end) {
        try {
          // Calculation of longitude band means.
          for (size_t iy = y_start; iy < y_end; ++iy) {
            auto acc = boost::accumulators::accumulator_set<
                Type,
                boost::accumulators::stats<boost::accumulators::tag::count,
                                           boost::accumulators::tag::mean>>();
            for (int64_t ix = 0; ix < grid.rows(); ++ix) {
              if (!mask(ix, iy)) {
                acc(grid(ix, iy));
              }
            }

            // The masked value is replaced by the average of the longitude band
            // if it is defined; otherwise it is replaced by zero.
            auto first_guess = boost::accumulators::count(acc)
                                   ? boost::accumulators::mean(acc)
                                   : Type(0);
            for (int64_t ix = 0; ix < grid.rows(); ++ix) {
              if (mask(ix, iy)) {
                grid(ix, iy) = first_guess;
              }
            }
          }
        } catch (...) {
          except = std::current_exception();
        }
      },
      grid.cols(), num_threads);

  if (except != nullptr) {
    std::rethrow_exception(except);
  }
}

///  Replaces all undefined values (NaN) in a grid using the Gauss-Seidel
///  method by relaxation.
///
/// @param grid The grid to be processed
/// @param is_circle True if the X axis of the grid defines a circle.
/// @param relaxation Relaxation constant
/// @return maximum residual value
template <typename Type>
auto gauss_seidel(
    pybind11::EigenDRef<Eigen::Matrix<Type, Eigen::Dynamic, Eigen::Dynamic>>&
        grid,
    Eigen::Matrix<bool, -1, -1>& mask, const bool is_circle,
    const Type relaxation, const size_t num_threads) -> Type {
  // Maximum residual values for each thread.
  std::vector<Type> max_residuals(num_threads);

  // Shape of the grid
  auto x_size = grid.rows();
  auto y_size = grid.cols();

  // Captures the detected exceptions in the calculation function
  // (only the last exception captured is kept)
  auto except = std::exception_ptr(nullptr);

  // Gets the index of the pixel (ix, iy) in the matrix.
  auto coordinates = [](const int64_t ix, const int64_t iy,
                        const int64_t size) -> int64_t {
    return iy * size + ix;
  };

  // Thread worker responsible for processing several strips along the y-axis
  // of the grid.
  //
  // @param y_start First index y of the band to be processed.
  // @param y_end Last index y, excluded, of the band to be processed.
  // @param max_residual Maximum residual of this strip.
  // @param pipe_out Last index to be processed on in this band.
  // @param pipe_in Last index processed in the previous band.
  auto worker = [&](int64_t y_start, int64_t y_end, Type* max_residual,
                    std::atomic<int64_t>* pipe_out,
                    std::atomic<int64_t>* pipe_in) -> void {
    // Modifies the value of a masked pixel.
    auto cell_fill = [&grid, &relaxation, &max_residual](
                         int64_t ix0, int64_t ix, int64_t ix1, int64_t iy0,
                         int64_t iy, int64_t iy1) {
      auto residual = (Type(0.25) * (grid(ix0, iy) + grid(ix1, iy) +
                                     grid(ix, iy0) + grid(ix, iy1)) -
                       grid(ix, iy)) *
                      relaxation;
      grid(ix, iy) += residual;
      *max_residual = std::max(*max_residual, std::fabs(residual));
    };

    // Initialization of the maximum value of the residuals of the treated
    // strips.
    *max_residual = Type(0);

    try {
      for (auto ix = 0; ix < x_size; ++ix) {
        //
        auto ix0 = ix == 0 ? (is_circle ? x_size - 1 : 1) : ix - 1;
        auto ix1 = ix == x_size - 1 ? (is_circle ? 0 : x_size - 2) : ix + 1;

        // If necessary, check that the last index required for this block has
        // been processed in the previous band.
        if (pipe_in) {
          auto next_coordinates = coordinates(ix, y_start, y_size);
          while (*pipe_in < next_coordinates) {
            std::this_thread::sleep_for(std::chrono::nanoseconds(5));
          }
        }

        for (auto iy = y_start; iy < y_end; ++iy) {
          auto iy0 = iy == 0 ? 1 : iy - 1;
          auto iy1 = iy == y_size - 1 ? y_size - 2 : iy + 1;
          if (mask(ix, iy)) {
            cell_fill(ix0, ix, ix1, iy0, iy, iy1);
          }
        }

        // If necessary, the other thread responsible for processing the next
        // band is notified.
        if (pipe_out) {
          *pipe_out = coordinates(ix, y_end, y_size);
        }
      }
    } catch (...) {
      except = std::current_exception();
    }
  };

  if (num_threads == 1) {
    worker(0, y_size, &max_residuals[0], nullptr, nullptr);
  } else {
    assert(num_threads >= 2);
    std::vector<std::atomic<int64_t>> pipeline(num_threads);
    std::vector<std::thread> threads;

    int64_t start = 0;
    int64_t shift = y_size / num_threads;

    for (auto& item : pipeline) {
      item = std::numeric_limits<int>::min();
    }

    for (size_t index = 0; index < num_threads - 1; ++index) {
      threads.emplace_back(std::thread(
          worker, start, start + shift, &max_residuals[index], &pipeline[index],
          index == 0 ? nullptr : &pipeline[index - 1]));
      start += shift;
    }
    threads.emplace_back(std::thread(worker, start, y_size,
                                     &max_residuals[num_threads - 1], nullptr,
                                     &pipeline[num_threads - 2]));
    for (auto&& item : threads) {
      item.join();
    }
  }
  if (except != nullptr) {
    std::rethrow_exception(except);
  }
  return *std::max_element(max_residuals.begin(), max_residuals.end());
}

}  // namespace detail

namespace fill {

/// Type of first guess grid.
enum FirstGuess {
  kZero,          //!< Use 0.0 as an initial guess
  kZonalAverage,  //!< Use zonal average in x direction
};

/// Replaces all undefined values (NaN) in a grid using the Gauss-Seidel
/// method by relaxation.
///
/// @param grid The grid to be processed
/// @param is_circle True if the X axis of the grid defines a circle.
/// @param max_iterations Maximum number of iterations to be used by relaxation.
/// @param epsilon Tolerance for ending relaxation before the maximum number of
/// iterations limit.
/// @param relaxation Relaxation constant
/// @param num_threads The number of threads to use for the computation. If 0
/// all CPUs are used. If 1 is given, no parallel computing code is used at all,
/// which is useful for debugging.
/// @return A tuple containing the number of iterations performed and the
/// maximum residual value.
template <typename Type>
auto gauss_seidel(
    pybind11::EigenDRef<Eigen::Matrix<Type, Eigen::Dynamic, Eigen::Dynamic>>&
        grid,
    const FirstGuess first_guess, const bool is_circle,
    const size_t max_iterations, const Type epsilon, const Type relaxation,
    size_t num_threads) -> std::tuple<size_t, Type> {
  /// If the grid doesn't have an undefined value, this routine has nothing more
  /// to do.
  if (!grid.hasNaN()) {
    return std::make_tuple(0, Type(0));
  }

  /// Calculation of the maximum number of threads if the user chooses.
  if (num_threads == 0) {
    num_threads = std::thread::hardware_concurrency();
  }

  /// Calculation of the position of the undefined values on the grid.
  auto mask =
      Eigen::Matrix<bool, Eigen::Dynamic, Eigen::Dynamic>(grid.array().isNaN());

  /// Calculation of the first guess with the chosen method
  switch (first_guess) {
    case FirstGuess::kZero:
      grid = (mask.array()).select(0, grid);
      break;
    case FirstGuess::kZonalAverage:
      detail::set_zonal_average(grid, mask, num_threads);
      break;
    default:
      throw std::invalid_argument("Invalid guess type: " +
                                  std::to_string(first_guess));
  }

  // Initialization of the function results.
  size_t iteration = 0;
  Type max_residual = 0;

  for (size_t it = 0; it < max_iterations; ++it) {
    ++iteration;
    max_residual = detail::gauss_seidel<Type>(grid, mask, is_circle, relaxation,
                                              num_threads);
    if (max_residual < epsilon) {
      break;
    }
  }
  return std::make_tuple(iteration, max_residual);
}

/// Fills undefined values using a locally weighted regression function or
/// LOESS. The weight function used for LOESS is the tri-cube weight function,
/// w(x)=(1-|d|^{3})^{3}
///
/// @param grid Grid Function on a uniform 2-dimensional grid to be filled.
/// @param nx Number of points of the half-window to be taken into account along
/// the longitude axis.
/// @param nx Number of points of the half-window to be taken into account along
/// the latitude axis.
/// @param num_threads The number of threads to use for the computation. If 0
/// all CPUs are used. If 1 is given, no parallel computing code is used at all,
/// which is useful for debugging.
/// @return The grid will have all the NaN filled with extrapolated values.
template <typename Type>
auto loess(const Grid2D<Type>& grid, const uint32_t nx, const uint32_t ny,
           const size_t num_threads) -> pybind11::array_t<Type> {
  auto result = pybind11::array_t<Type>(
      pybind11::array::ShapeContainer{grid.x()->size(), grid.y()->size()});
  auto _result = result.template mutable_unchecked<2>();

  // Captures the detected exceptions in the calculation function
  // (only the last exception captured is kept)
  auto except = std::exception_ptr(nullptr);

  auto worker = [&](const size_t start, const size_t end) {
    try {
      for (size_t ix = start; ix < end; ++ix) {
        auto x = (*grid.x())(ix);

        for (int64_t iy = 0; iy < grid.y()->size(); ++iy) {
          auto z = grid.value(ix, iy);

          // If the current value is masked.
          if (std::isnan(z)) {
            auto y = (*grid.y())(iy);

            // Reading the coordinates of the window around the masked point.
            auto x_frame = grid.x()->find_indexes(x, nx, Axis::kSym);
            auto y_frame = grid.y()->find_indexes(y, ny, Axis::kSym);

            // Initialization of values to calculate the extrapolated value.
            auto value = Type(0);
            auto weight = Type(0);

            // For all the coordinates of the frame.
            for (auto wx : x_frame) {
              for (auto wy : y_frame) {
                auto zi = grid.value(wx, wy);

                // If the value is not masked, its weight is calculated from the
                // tri-cube weight function
                if (!std::isnan(zi)) {
                  const auto power = 3.0;
                  auto d =
                      std::sqrt(detail::math::sqr((((*grid.x())(wx)-x)) / nx) +
                                detail::math::sqr((((*grid.y())(wy)-y)) / ny));
                  auto wi = d <= 1 ? std::pow((1.0 - std::pow(d, power)), power)
                                   : 0.0;
                  value += static_cast<Type>(wi * zi);
                  weight += static_cast<Type>(wi);
                }
              }
            }

            // Finally, we calculate the extrapolated value if possible,
            // otherwise we will recopy the masked original value.
            if (weight != 0) {
              z = value / weight;
            }
          }
          _result(ix, iy) = z;
        }
      }
    } catch (...) {
      except = std::current_exception();
    }
  };

  {
    pybind11::gil_scoped_release release;
    detail::dispatch(worker, grid.x()->size(), num_threads);
  }
  return result;
}

}  // namespace fill
}  // namespace pyinterp
