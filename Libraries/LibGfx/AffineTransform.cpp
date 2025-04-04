/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/Optional.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/Quad.h>
#include <LibGfx/Rect.h>

namespace Gfx {

template<FloatingPoint T>
T AffineTransform<T>::x_scale() const
{
    return AK::hypot(m_values[0], m_values[1]);
}

template<FloatingPoint T>
T AffineTransform<T>::y_scale() const
{
    return AK::hypot(m_values[2], m_values[3]);
}

template<FloatingPoint T>
Point<T> AffineTransform<T>::translation() const
{
    return { x_translation(), y_translation() };
}

template<FloatingPoint T>
AffineTransform<T>& AffineTransform<T>::scale(T sx, T sy)
{
    m_values[0] *= sx;
    m_values[1] *= sx;
    m_values[2] *= sy;
    m_values[3] *= sy;
    return *this;
}

template<FloatingPoint T>
AffineTransform<T>& AffineTransform<T>::scale(Point<T> s)
{
    return scale(s.x(), s.y());
}

template<FloatingPoint T>
AffineTransform<T>& AffineTransform<T>::set_scale(T sx, T sy)
{
    m_values[0] = sx;
    m_values[1] = 0;
    m_values[2] = 0;
    m_values[3] = sy;
    return *this;
}

template<FloatingPoint T>
AffineTransform<T>& AffineTransform<T>::set_scale(Point<T> s)
{
    return set_scale(s.x(), s.y());
}

template<FloatingPoint T>
AffineTransform<T>& AffineTransform<T>::skew_radians(T x_radians, T y_radians)
{
    AffineTransform skew_transform(1, AK::tan(y_radians), AK::tan(x_radians), 1, 0, 0);
    multiply(skew_transform);
    return *this;
}

template<FloatingPoint T>
AffineTransform<T>& AffineTransform<T>::translate(T tx, T ty)
{
    if (is_identity_or_translation()) {
        m_values[4] += tx;
        m_values[5] += ty;
        return *this;
    }
    m_values[4] += tx * m_values[0] + ty * m_values[2];
    m_values[5] += tx * m_values[1] + ty * m_values[3];
    return *this;
}

template<FloatingPoint T>
AffineTransform<T>& AffineTransform<T>::translate(Point<T> t)
{
    return translate(t.x(), t.y());
}

template<FloatingPoint T>
AffineTransform<T>& AffineTransform<T>::set_translation(T tx, T ty)
{
    m_values[4] = tx;
    m_values[5] = ty;
    return *this;
}

template<FloatingPoint T>
AffineTransform<T>& AffineTransform<T>::set_translation(Point<T> t)
{
    return set_translation(t.x(), t.y());
}

template<FloatingPoint T>
AffineTransform<T>& AffineTransform<T>::multiply(AffineTransform const& other)
{
    if (other.is_identity())
        return *this;
    AffineTransform result;
    result.m_values[0] = other.a() * a() + other.b() * c();
    result.m_values[1] = other.a() * b() + other.b() * d();
    result.m_values[2] = other.c() * a() + other.d() * c();
    result.m_values[3] = other.c() * b() + other.d() * d();
    result.m_values[4] = other.e() * a() + other.f() * c() + e();
    result.m_values[5] = other.e() * b() + other.f() * d() + f();
    *this = result;
    return *this;
}

template<FloatingPoint T>
AffineTransform<T>& AffineTransform<T>::rotate_radians(T radians)
{
    T sin_angle;
    T cos_angle;
    AK::sincos(radians, sin_angle, cos_angle);
    AffineTransform rotation(cos_angle, sin_angle, -sin_angle, cos_angle, 0, 0);
    multiply(rotation);
    return *this;
}

template<FloatingPoint T>
T AffineTransform<T>::determinant() const
{
    return a() * d() - b() * c();
}

template<FloatingPoint T>
Optional<AffineTransform<T>> AffineTransform<T>::inverse() const
{
    auto det = determinant();
    if (det == 0)
        return {};
    return AffineTransform {
        d() / det,
        -b() / det,
        -c() / det,
        a() / det,
        (c() * f() - d() * e()) / det,
        (b() * e() - a() * f()) / det,
    };
}

template<FloatingPoint T>
void AffineTransform<T>::map(T unmapped_x, T unmapped_y, T& mapped_x, T& mapped_y) const
{
    mapped_x = a() * unmapped_x + c() * unmapped_y + e();
    mapped_y = b() * unmapped_x + d() * unmapped_y + f();
}

template<FloatingPoint T>
template<Integral U>
Point<U> AffineTransform<T>::map(Point<U> point) const
{
    T mapped_x;
    T mapped_y;
    map(static_cast<T>(point.x()), static_cast<T>(point.y()), mapped_x, mapped_y);
    return { round_to<U>(mapped_x), round_to<U>(mapped_y) };
}

template<FloatingPoint T>
Point<T> AffineTransform<T>::map(Point<T> point) const
{
    T mapped_x;
    T mapped_y;
    map(point.x(), point.y(), mapped_x, mapped_y);
    return { mapped_x, mapped_y };
}

template<FloatingPoint T>
template<Integral U>
Size<U> AffineTransform<T>::map(Size<U> size) const
{
    return {
        round_to<U>(static_cast<T>(size.width()) * x_scale()),
        round_to<U>(static_cast<T>(size.height()) * y_scale()),
    };
}

template<FloatingPoint T>
Size<T> AffineTransform<T>::map(Size<T> size) const
{
    return { size.width() * x_scale(), size.height() * y_scale() };
}

template<typename T>
static T smallest_of(T p1, T p2, T p3, T p4)
{
    return min(min(p1, p2), min(p3, p4));
}

template<typename T>
static T largest_of(T p1, T p2, T p3, T p4)
{
    return max(max(p1, p2), max(p3, p4));
}

template<FloatingPoint T>
template<Integral U>
Rect<U> AffineTransform<T>::map(Rect<U> const& rect) const
{
    return enclosing_int_rect(map(Rect<T>(rect)));
}

template<FloatingPoint T>
Rect<T> AffineTransform<T>::map(Rect<T> const& rect) const
{
    if (is_identity())
        return rect;

    if (is_identity_or_translation())
        return rect.translated(e(), f());

    auto p1 = map(rect.top_left());
    auto p2 = map(rect.top_right());
    auto p3 = map(rect.bottom_right());
    auto p4 = map(rect.bottom_left());
    auto left = smallest_of(p1.x(), p2.x(), p3.x(), p4.x());
    auto top = smallest_of(p1.y(), p2.y(), p3.y(), p4.y());
    auto right = largest_of(p1.x(), p2.x(), p3.x(), p4.x());
    auto bottom = largest_of(p1.y(), p2.y(), p3.y(), p4.y());
    return { left, top, right - left, bottom - top };
}

template<FloatingPoint T>
Quad<T> AffineTransform<T>::map_to_quad(Rect<T> const& rect) const
{
    return {
        map(rect.top_left()),
        map(rect.top_right()),
        map(rect.bottom_right()),
        map(rect.bottom_left()),
    };
}

template<FloatingPoint T>
T AffineTransform<T>::rotation() const
{
    auto rotation = AK::atan2(b(), a());
    while (rotation < -AK::Pi<T>)
        rotation += 2.0f * AK::Pi<T>;
    while (rotation > AK::Pi<T>)
        rotation -= 2.0f * AK::Pi<T>;
    return rotation;
}

}

template class Gfx::AffineTransform<float>;
template class Gfx::AffineTransform<double>;
