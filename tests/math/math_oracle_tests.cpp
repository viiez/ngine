// Vergleicht ngine.math mit GLM (ADR 0025): GLM dient nur noch hier als Orakel. Die Eingaben sind
// zufällig, aber mit festem Startwert, damit ein Fehlschlag reproduzierbar bleibt.
#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstring>
#include <random>

import ngine.core;
import ngine.math;

using namespace ngine;

namespace {

static_assert(sizeof(math::Vec3) == sizeof(glm::vec3));
static_assert(sizeof(math::Mat4) == sizeof(glm::mat4));
static_assert(sizeof(math::Quat) == sizeof(glm::quat));

// Gleiches Speicherlayout: Felder bzw. Spalten in derselben Reihenfolge.
template <typename To, typename From>
To same_layout(const From& from) {
    static_assert(sizeof(To) == sizeof(From));
    To to;
    std::memcpy(&to, &from, sizeof(To));
    return to;
}

struct Random {
    std::mt19937 engine{20261103};

    f32 number(f32 low = -10.0f, f32 high = 10.0f) {
        return std::uniform_real_distribution<f32>{low, high}(engine);
    }
    math::Vec3 vec3() {
        return {number(), number(), number()};
    }
    math::Vec3 direction() {
        math::Vec3 v;
        do {
            v = vec3();
        } while (math::length(v) < 0.1f);
        return math::normalize(v);
    }
    math::Quat rotation() {
        return math::angle_axis(number(-math::pi, math::pi), direction());
    }
    math::Mat4 transform() {
        return math::compose(vec3(), rotation(),
                             {number(0.2f, 3.0f), number(0.2f, 3.0f), number(0.2f, 3.0f)});
    }
};

glm::vec3 g(math::Vec3 v) {
    return {v.x, v.y, v.z};
}
glm::quat g(math::Quat q) {
    return glm::quat{q.w, q.x, q.y, q.z};
}
glm::mat4 g(const math::Mat4& m) {
    return same_layout<glm::mat4>(m);
}
math::Mat4 n(const glm::mat4& m) {
    return same_layout<math::Mat4>(m);
}
math::Mat3 n(const glm::mat3& m) {
    return same_layout<math::Mat3>(m);
}
math::Vec3 n(glm::vec3 v) {
    return {v.x, v.y, v.z};
}

constexpr int runs = 200;

} // namespace

TEST_CASE("Matrix products, inverse and determinant match GLM", "[math]") {
    Random random;
    for (int i = 0; i < runs; ++i) {
        const math::Mat4 a = random.transform();
        const math::Mat4 b = random.transform();
        CHECK(math::near(a * b, n(g(a) * g(b)), 1e-3f));
        CHECK(math::near(math::inverse(a), n(glm::inverse(g(a))), 1e-4f));
        CHECK(math::near(math::determinant(math::to_mat3(a)), glm::determinant(glm::mat3{g(a)}),
                         1e-3f));
        CHECK(math::near(math::inverse(math::to_mat3(a)), n(glm::inverse(glm::mat3{g(a)})), 1e-4f));
        CHECK(math::near(math::transpose(a), n(glm::transpose(g(a))), 0.0f));
        // Allgemeine Matrix ohne affine Gestalt.
        math::Mat4 c = a;
        c[0].w = random.number(-1.0f, 1.0f);
        c[2].w = random.number(-1.0f, 1.0f);
        CHECK(math::near(math::determinant(c), glm::determinant(g(c)), 1e-2f));
        CHECK(math::near(math::inverse(c), n(glm::inverse(g(c))), 1e-3f));
    }
}

TEST_CASE("Quaternions match GLM", "[math]") {
    Random random;
    for (int i = 0; i < runs; ++i) {
        const f32 angle = random.number(-math::pi, math::pi);
        const math::Vec3 axis = random.direction();
        const math::Quat a = math::angle_axis(angle, axis);
        const math::Quat b = random.rotation();
        const math::Vec3 v = random.vec3();
        CHECK(math::near(a, same_layout<math::Quat>(glm::angleAxis(angle, g(axis))), 1e-6f));
        CHECK(math::near(a * v, n(g(a) * g(v)), 1e-4f));
        CHECK(math::near(a * b, same_layout<math::Quat>(g(a) * g(b)), 1e-5f));
        CHECK(math::near(math::to_mat3(a), n(glm::mat3_cast(g(a))), 1e-5f));
        CHECK(math::near(math::to_quat(math::to_mat3(a)), a, 1e-5f));
        const f32 t = random.number(0.0f, 1.0f);
        CHECK(math::near(math::slerp(a, b, t), same_layout<math::Quat>(glm::slerp(g(a), g(b), t)),
                         1e-4f));
        const math::Vec3 forward = random.direction();
        if (std::abs(forward.y) < 0.99f) {
            const math::Quat look = math::look_rotation(forward, {0, 1, 0});
            CHECK(math::near(
                look, same_layout<math::Quat>(glm::quatLookAtRH(g(forward), {0, 1, 0})), 1e-5f));
        }
    }
}

TEST_CASE("View and projection matrices match GLM", "[math]") {
    Random random;
    for (int i = 0; i < runs; ++i) {
        const math::Vec3 eye = random.vec3();
        const math::Vec3 target = eye + random.direction() * random.number(1.0f, 10.0f);
        if (std::abs(math::normalize(target - eye).y) > 0.99f) {
            continue;
        }
        CHECK(math::near(math::look_at(eye, target, {0, 1, 0}),
                         n(glm::lookAtRH(g(eye), g(target), {0, 1, 0})), 1e-4f));

        const f32 fov = random.number(0.3f, 2.0f);
        const f32 aspect = random.number(0.5f, 3.0f);
        const f32 z_near = random.number(0.01f, 1.0f);
        const f32 z_far = random.number(10.0f, 1000.0f);
        CHECK(math::near(math::perspective(fov, aspect, z_near, z_far),
                         n(glm::perspectiveRH_ZO(fov, aspect, z_near, z_far)), 1e-4f));
        CHECK(math::near(math::orthographic(-aspect, aspect, -1.0f, 2.0f, z_near, z_far),
                         n(glm::orthoRH_ZO(-aspect, aspect, -1.0f, 2.0f, z_near, z_far)), 1e-5f));
    }
}

TEST_CASE("Vector functions match GLM", "[math]") {
    Random random;
    for (int i = 0; i < runs; ++i) {
        const math::Vec3 a = random.vec3();
        const math::Vec3 b = random.vec3();
        CHECK(math::near(math::dot(a, b), glm::dot(g(a), g(b)), 1e-4f));
        CHECK(math::near(math::cross(a, b), n(glm::cross(g(a), g(b))), 1e-4f));
        CHECK(math::near(math::length(a), glm::length(g(a)), 1e-5f));
        CHECK(math::near(math::normalize(a), n(glm::normalize(g(a))), 1e-6f));
        CHECK(math::near(math::distance(a, b), glm::distance(g(a), g(b)), 1e-5f));
    }
    // Abweichung mit Absicht: Der Nullvektor bleibt beim Normieren der Nullvektor.
    CHECK(math::normalize(math::Vec3{}) == math::Vec3{});
}
