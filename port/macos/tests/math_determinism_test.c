/*
MATH_DETERMINISM_TEST.C

System link games need every machine to compute the game's floating point
results bit for bit alike (port/include/halo_math.h). This guest image runs
the game's own maths - source/math/matrix_math.c and the halo_ functions of
port/third_party/musl-math - over a million pseudo-random inputs and prints
a hash of every result's bits. Builds that print the same hash compute
alike: the native build against the x86-64 one, or a build with new
compiler flags or hand-written SIMD against the one before
(port/macos/tests/run_determinism_test.sh).
*/

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* the game's types (source/math/real_math.h), spelled out: this file is
not compiled with the game's headers */
typedef struct { float scale; float n[4][3]; } matrix4x3;
typedef struct { float i, j, k, w; } quaternion;
typedef struct { float x, y, z; } vector3;

void matrix4x3_multiply(const matrix4x3 *a, const matrix4x3 *b, matrix4x3 *result);
vector3 *matrix4x3_transform_point(const matrix4x3 *matrix, const vector3 *point, vector3 *result);
vector3 *matrix4x3_transform_vector(const matrix4x3 *matrix, const vector3 *vector, vector3 *result);
vector3 *matrix4x3_inverse_transform_point(const matrix4x3 *matrix, const vector3 *point, vector3 *result);
vector3 *matrix4x3_transform_normal(const matrix4x3 *matrix, const vector3 *normal, vector3 *result);
void matrix4x3_inverse(const matrix4x3 *matrix, matrix4x3 *result);
void matrix4x3_rotation_from_quaternion(matrix4x3 *matrix, const quaternion *quaternion);
void matrix4x3_rotation_to_quaternion(const matrix4x3 *matrix, quaternion *quaternion);
void matrix4x3_rotation_from_angles(matrix4x3 *matrix, float yaw, float pitch, float roll);

double halo_sin(double x);
double halo_cos(double x);
double halo_tan(double x);
double halo_asin(double x);
double halo_acos(double x);
double halo_atan(double x);
double halo_atan2(double y, double x);
double halo_exp(double x);
double halo_log(double x);
double halo_pow(double x, double y);

static uint64_t hash = 1469598103934665603ULL;
static uint32_t seed = 20260929;

static void mix(const void *data, size_t size)
{
	const unsigned char *bytes = data;
	size_t index;

	for (index = 0; index < size; index++)
	{
		hash ^= bytes[index];
		hash *= 1099511628211ULL;
	}
}

static float random_real(float low, float high)
{
	seed = seed * 1664525u + 1013904223u;
	return low + (high - low) * (float)(seed >> 8) / 16777216.0f;
}

static void random_rotation(matrix4x3 *matrix)
{
	matrix4x3_rotation_from_angles(matrix, random_real(-3.2f, 3.2f), random_real(-1.6f, 1.6f),
		random_real(-3.2f, 3.2f));
	matrix->scale = random_real(0.25f, 4.0f);
	matrix->n[3][0] = random_real(-500.0f, 500.0f);
	matrix->n[3][1] = random_real(-500.0f, 500.0f);
	matrix->n[3][2] = random_real(-50.0f, 50.0f);
}

int main(void)
{
	int round, section;
	const char *names[] = { "matrix4x3", "transcendental" };
	uint64_t section_hashes[2];

	for (section = 0; section < 2; section++)
	{
		hash = 1469598103934665603ULL;
		for (round = 0; round < (section == 0 ? 200000 : 400000); round++)
		{
			if (section == 0)
			{
				matrix4x3 a, b, product, inverse;
				vector3 point = { random_real(-1000, 1000), random_real(-1000, 1000), random_real(-100, 100) };
				vector3 out;
				quaternion q;

				random_rotation(&a);
				random_rotation(&b);
				mix(&a, sizeof(a));
				matrix4x3_multiply(&a, &b, &product);
				mix(&product, sizeof(product));
				matrix4x3_inverse(&product, &inverse);
				mix(&inverse, sizeof(inverse));
				mix(matrix4x3_transform_point(&product, &point, &out), sizeof(out));
				mix(matrix4x3_transform_vector(&product, &point, &out), sizeof(out));
				mix(matrix4x3_inverse_transform_point(&product, &point, &out), sizeof(out));
				mix(matrix4x3_transform_normal(&a, &point, &out), sizeof(out));
				matrix4x3_rotation_to_quaternion(&a, &q);
				mix(&q, sizeof(q));
				matrix4x3_rotation_from_quaternion(&b, &q);
				mix(&b, sizeof(b));
			}
			else
			{
				double x = random_real(-10.0f, 10.0f), y = random_real(-10.0f, 10.0f), results[10];

				results[0] = halo_sin(x);
				results[1] = halo_cos(x);
				results[2] = halo_tan(x * 0.15);
				results[3] = halo_asin(x * 0.0999);
				results[4] = halo_acos(y * 0.0999);
				results[5] = halo_atan(x);
				results[6] = halo_atan2(y, x);
				results[7] = halo_exp(x);
				results[8] = halo_log(x * x + 0.001);
				results[9] = halo_pow(x * x + 0.5, y * 0.5);
				mix(results, sizeof(results));
			}
		}
		section_hashes[section] = hash;
		printf("%-15s %016llx\n", names[section], (unsigned long long)hash);
	}
	hash = section_hashes[0] ^ (section_hashes[1] * 31);
	printf("all             %016llx\n", (unsigned long long)hash);
	return 0;
}
