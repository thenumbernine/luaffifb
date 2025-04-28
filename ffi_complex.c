#include "ffi_complex.h"

#ifdef HAVE_COMPLEX

#if 1
complex_double mk_complex_double(double real, double imag) {
	return real + imag * 1i;
}
#else
complex_double mk_complex_double(double real, double imag) {
	return __builtin_complex(real, imag);
}
#endif

complex_double mk_complex_float(double real, double imag) { 
	return real + imag * 1i; 
}

#else

complex_double mk_complex_double(double real, double imag) { 
	return complex_double{ real, imag }; 
}
complex_float mk_complex_float(double real, double imag) { 
	return complex_float{ real, imag }; 
}

#endif
