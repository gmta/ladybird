/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Format.h>
#include <AK/Forward.h>
#include <LibGfx/Forward.h>

namespace Gfx {

template<FloatingPoint T>
class AffineTransform {
public:
    AffineTransform()
        : m_values { 1, 0, 0, 1, 0, 0 }
    {
    }

    AffineTransform(T a, T b, T c, T d, T e, T f)
        : m_values { a, b, c, d, e, f }
    {
    }

    [[nodiscard]] bool is_identity() const
    {
        return m_values[0] == 1 && m_values[1] == 0 && m_values[2] == 0 && m_values[3] == 1 && m_values[4] == 0 && m_values[5] == 0;
    }

    [[nodiscard]] bool is_identity_or_translation() const
    {
        return m_values[0] == 1 && m_values[1] == 0 && m_values[2] == 0 && m_values[3] == 1;
    }

    [[nodiscard]] bool is_identity_or_translation_or_scale() const
    {
        return m_values[1] == 0 && m_values[2] == 0;
    }

    void map(T unmapped_x, T unmapped_y, T& mapped_x, T& mapped_y) const;

    template<Integral U>
    Point<U> map(Point<U>) const;

    Point<T> map(Point<T>) const;

    template<Integral U>
    Size<U> map(Size<U>) const;

    Size<T> map(Size<T>) const;

    template<Integral U>
    Rect<U> map(Rect<U> const&) const;

    Rect<T> map(Rect<T> const&) const;

    Quad<T> map_to_quad(Rect<T> const&) const;

    [[nodiscard]] ALWAYS_INLINE T a() const { return m_values[0]; }
    [[nodiscard]] ALWAYS_INLINE T b() const { return m_values[1]; }
    [[nodiscard]] ALWAYS_INLINE T c() const { return m_values[2]; }
    [[nodiscard]] ALWAYS_INLINE T d() const { return m_values[3]; }
    [[nodiscard]] ALWAYS_INLINE T e() const { return m_values[4]; }
    [[nodiscard]] ALWAYS_INLINE T f() const { return m_values[5]; }

    [[nodiscard]] T x_scale() const;
    [[nodiscard]] T y_scale() const;
    [[nodiscard]] Point<T> scale() const { return { x_scale(), y_scale() }; }
    [[nodiscard]] T x_translation() const { return e(); }
    [[nodiscard]] T y_translation() const { return f(); }
    [[nodiscard]] Point<T> translation() const;
    [[nodiscard]] T rotation() const;

    AffineTransform& scale(T sx, T sy);
    AffineTransform& scale(Point<T> s);
    AffineTransform& set_scale(T sx, T sy);
    AffineTransform& set_scale(Point<T> s);
    AffineTransform& translate(T tx, T ty);
    AffineTransform& translate(Point<T> t);
    AffineTransform& set_translation(T tx, T ty);
    AffineTransform& set_translation(Point<T> t);
    AffineTransform& rotate_radians(T);
    AffineTransform& skew_radians(T x_radians, T y_radians);
    AffineTransform& multiply(AffineTransform const&);

    T determinant() const;
    Optional<AffineTransform> inverse() const;

    template<FloatingPoint U>
    requires(!IsSame<T, U>)
    [[nodiscard]] ALWAYS_INLINE AffineTransform<U> to_type()
    {
        return AffineTransform<U> {
            static_cast<U>(a()),
            static_cast<U>(b()),
            static_cast<U>(c()),
            static_cast<U>(d()),
            static_cast<U>(e()),
            static_cast<U>(f()),
        };
    }

private:
    T m_values[6] { 0 };
};

using FloatAffineTransform = AffineTransform<float>;
using DoubleAffineTransform = AffineTransform<double>;

}

template<FloatingPoint T>
struct AK::Formatter<Gfx::AffineTransform<T>> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Gfx::AffineTransform<T> const& value)
    {
        return Formatter<FormatString>::format(builder, "[{} {} {} {} {} {}]"sv, value.a(), value.b(), value.c(), value.d(), value.e(), value.f());
    }
};
