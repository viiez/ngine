#include <catch2/catch_test_macros.hpp>

import ngine.core;
import ngine.math;

using namespace ngine;
using namespace ngine::math;

TEST_CASE("Default constructed values are zero or identity", "[math]") {
    CHECK(Vec3{} == Vec3{0.0f, 0.0f, 0.0f});
    CHECK(DVec3{} == DVec3{0.0, 0.0, 0.0});
    CHECK(near(Mat4{}, mat4_identity(), 0.0f));
    const Quat q{};
    CHECK(q.w == 1.0f);
    CHECK(q.x == 0.0f);
    CHECK(near(Affine{}.linear, mat3_identity(), 0.0f));
    CHECK(near(quat_identity(), Quat{}, 0.0f));
}

TEST_CASE("Vector operations", "[math]") {
    const Vec3 a{1.0f, 2.0f, 3.0f};
    const Vec3 b{4.0f, 5.0f, 6.0f};
    CHECK(a + b == Vec3{5.0f, 7.0f, 9.0f});
    CHECK(b - a == Vec3{3.0f, 3.0f, 3.0f});
    CHECK(a * 2.0f == Vec3{2.0f, 4.0f, 6.0f});
    CHECK(dot(a, b) == 32.0f);
    CHECK(cross(Vec3{1, 0, 0}, Vec3{0, 1, 0}) == Vec3{0, 0, 1});
    CHECK(length(Vec3{3, 4, 0}) == 5.0f);
    CHECK(length_squared(Vec3{3, 4, 0}) == 25.0f);
    CHECK(near(normalize(Vec3{0, 0, 7}), Vec3{0, 0, 1}, 1e-6f));
    CHECK(near(lerp(a, b, 0.5f), Vec3{2.5f, 3.5f, 4.5f}, 1e-6f));
    CHECK(to_f32(DVec3{1.5, 2.5, 3.5}) == Vec3{1.5f, 2.5f, 3.5f});
    CHECK(to_f64(Vec3{1.5f, 2.5f, 3.5f}) == DVec3{1.5, 2.5, 3.5});
    CHECK(xyz(extend(a, 4.0f)) == a);
}

TEST_CASE("Quaternions rotate counter clockwise around their axis", "[math]") {
    const Quat q = angle_axis(radians(90.0f), Vec3{0, 1, 0});
    // Rechtshändig: +x dreht um +y auf -z.
    CHECK(near(q * Vec3{1, 0, 0}, Vec3{0, 0, -1}, 1e-6f));
    CHECK(near(to_mat3(q) * Vec3{1, 0, 0}, Vec3{0, 0, -1}, 1e-6f));
    CHECK(near(q * q, angle_axis(radians(180.0f), Vec3{0, 1, 0}), 1e-6f));
    CHECK(near(inverse(q) * (q * Vec3{1, 2, 3}), Vec3{1, 2, 3}, 1e-5f));
    CHECK(near(slerp(quat_identity(), q, 0.5f), angle_axis(radians(45.0f), Vec3{0, 1, 0}), 1e-6f));
}

TEST_CASE("Euler angles round trip in yaw, pitch, roll order", "[math]") {
    const Vec3 angles{radians(20.0f), radians(-130.0f), radians(75.0f)};
    const Quat q = from_euler(angles);
    CHECK(near(q,
               angle_axis(angles.y, Vec3{0, 1, 0}) * angle_axis(angles.x, Vec3{1, 0, 0}) *
                   angle_axis(angles.z, Vec3{0, 0, 1}),
               1e-6f));
    CHECK(near(to_euler(q), angles, 1e-5f));
    CHECK(near(to_euler(quat_identity()), Vec3{}, 0.0f));
    // Gimbal Lock: pitch 90 Grad, roll geht in yaw auf, die Drehung bleibt gleich.
    const Quat locked = from_euler({radians(90.0f), radians(30.0f), radians(20.0f)});
    CHECK(near(from_euler(to_euler(locked)), locked, 1e-5f));
}

TEST_CASE("look_rotation points minus z along the view direction", "[math]") {
    const Vec3 forward = normalize(Vec3{1, -1, -1});
    const Quat q = look_rotation(forward, Vec3{0, 1, 0});
    CHECK(near(q * Vec3{0, 0, -1}, forward, 1e-5f));
    // Die rechte Hand bleibt waagerecht.
    CHECK(near((q * Vec3{1, 0, 0}).y, 0.0f, 1e-5f));
}

TEST_CASE("compose scales, then rotates, then translates", "[math]") {
    const Mat4 m =
        compose(Vec3{10, 0, 0}, angle_axis(radians(90.0f), Vec3{0, 0, 1}), Vec3{2, 2, 2});
    // (1, 0, 0) -> skaliert (2, 0, 0) -> gedreht (0, 2, 0) -> verschoben (10, 2, 0)
    CHECK(near(m * Vec4{1, 0, 0, 1}, Vec4{10, 2, 0, 1}, 1e-5f));
    CHECK(near(inverse(m) * (m * Vec4{1, 2, 3, 1}), Vec4{1, 2, 3, 1}, 1e-5f));
}

TEST_CASE("Projections map to Vulkan depth from zero to one", "[math]") {
    const Mat4 view = look_at(Vec3{0, 0, 5}, Vec3{0, 0, 0}, Vec3{0, 1, 0});
    CHECK(near(view * Vec4{0, 0, 0, 1}, Vec4{0, 0, -5, 1}, 1e-6f));

    const Mat4 p = perspective(radians(60.0f), 16.0f / 9.0f, 0.5f, 100.0f);
    const Vec4 at_near = p * Vec4{0, 0, -0.5f, 1};
    const Vec4 at_far = p * Vec4{0, 0, -100.0f, 1};
    CHECK(near(at_near.z / at_near.w, 0.0f, 1e-6f));
    CHECK(near(at_far.z / at_far.w, 1.0f, 1e-5f));

    const Mat4 r = perspective_reverse_z(radians(60.0f), 16.0f / 9.0f, 0.5f);
    const Vec4 r_near = r * Vec4{0, 0, -0.5f, 1};
    const Vec4 r_far = r * Vec4{0, 0, -1.0e6f, 1};
    CHECK(near(r_near.z / r_near.w, 1.0f, 1e-6f));
    CHECK(r_far.z / r_far.w < 1e-6f);
    CHECK(r_far.z / r_far.w > 0.0f);
    // Gleiche Bildposition wie die normale Perspektive.
    const Vec4 side = Vec4{1, 1, -10, 1};
    CHECK(near((r * side).x / (r * side).w, (p * side).x / (p * side).w, 1e-6f));

    const Mat4 o = orthographic(-1, 1, -1, 1, 0, 10);
    CHECK(near((o * Vec4{0, 0, -10, 1}).z, 1.0f, 1e-6f));
}

TEST_CASE("Affine composes like matrices", "[math]") {
    const Affine parent = Affine::from(DVec3{100.0, 0.0, 0.0},
                                       angle_axis(radians(90.0f), Vec3{0, 1, 0}), Vec3{2, 2, 2});
    const Affine child = Affine::from(DVec3{1.0, 0.0, 0.0}, quat_identity(), Vec3{1, 1, 1});
    const Affine world = parent * child;
    // Kind bei x = 1 im Elternteil: skaliert auf 2, um y gedreht nach -z, verschoben um 100.
    CHECK(near(world.translation, DVec3{100.0, 0.0, -2.0}, 1e-6));
    CHECK(near(world.transform_point(DVec3{0, 0, 0}), parent.transform_point(DVec3{1.0, 0.0, 0.0}),
               1e-9));
    CHECK(near(inverse(world) * world, Affine{}, 1e-6f, 1e-9));
}

TEST_CASE("Far from the origin positions keep millimeter precision relative to the camera",
          "[math]") {
    // 10 000 km vom Ursprung: f32 hätte hier nur noch eine Auflösung von rund einem Meter.
    const DVec3 far{1.0e7, 2.0e3, -1.0e7};
    const DVec3 camera = far + DVec3{0.0, 1.7, 5.0};
    const Affine object =
        Affine::from(far + DVec3{0.001, 0.0, 0.0}, quat_identity(), Vec3{1, 1, 1});

    const Mat4 relative = object.relative_to(camera);
    CHECK(near(xyz(relative[3]), Vec3{0.001f, -1.7f, -5.0f}, 1e-6f));

    // Zum Vergleich: dieselbe Rechnung in 32 Bit verliert den Millimeter.
    const Vec3 far32 = to_f32(far + DVec3{0.001, 0.0, 0.0});
    const Vec3 camera32 = to_f32(camera);
    CHECK_FALSE(near((far32 - camera32).x, 0.001f, 1e-4f));
}
