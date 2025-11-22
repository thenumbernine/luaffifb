#pragma once

#include <complex.h>
#define HAVE_COMPLEX

#ifdef HAVE_COMPLEX

typedef double complex complex_double;
typedef float complex complex_float;
complex_double mk_complex_double(double real, double imag);
complex_double mk_complex_float(double real, double imag);

#else

typedef struct {
	double real, imag;
} complex_double;

typedef struct {
	float real, imag;
} complex_float;

complex_double mk_complex_double(double real, double imag);
complex_float mk_complex_float(double real, double imag);
inline double creal(complex_double c) { return c.real; }
inline float crealf(complex_float c) { return c.real; }
inline double cimag(complex_double c) { return c.imag; }
inline float cimagf(complex_float c) { return c.imag; }

#endif
