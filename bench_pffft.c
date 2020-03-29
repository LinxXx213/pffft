/*
  Copyright (c) 2013 Julien Pommier.

  Small test & bench for PFFFT, comparing its performance with the scalar FFTPACK, FFTW, and Apple vDSP

  How to build: 

  on linux, with fftw3:
  gcc -o test_pffft -DHAVE_FFTW -msse -mfpmath=sse -O3 -Wall -W pffft.c test_pffft.c fftpack.c -L/usr/local/lib -I/usr/local/include/ -lfftw3f -lm

  on macos, without fftw3:
  clang -o test_pffft -DHAVE_VECLIB -O3 -Wall -W pffft.c test_pffft.c fftpack.c -L/usr/local/lib -I/usr/local/include/ -framework Accelerate

  on macos, with fftw3:
  clang -o test_pffft -DHAVE_FFTW -DHAVE_VECLIB -O3 -Wall -W pffft.c test_pffft.c fftpack.c -L/usr/local/lib -I/usr/local/include/ -lfftw3f -framework Accelerate

  as alternative: replace clang by gcc.

  on windows, with visual c++:
  cl /Ox -D_USE_MATH_DEFINES /arch:SSE test_pffft.c pffft.c fftpack.c
  
  build without SIMD instructions:
  gcc -o test_pffft -DPFFFT_SIMD_DISABLE -O3 -Wall -W pffft.c test_pffft.c fftpack.c -lm

 */

#include "pffft.h"
#include "fftpack.h"

#ifdef HAVE_GREEN_FFTS
#include "fftext.h"
#endif

#ifdef HAVE_KISS_FFT
#include <kiss_fft.h>
#include <kiss_fftr.h>
#endif


#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <assert.h>
#include <string.h>

#ifdef HAVE_SYS_TIMES
#  include <sys/times.h>
#  include <unistd.h>
#endif

#ifdef HAVE_VECLIB
#  include <Accelerate/Accelerate.h>
#endif

#ifdef HAVE_FFTW
#  include <fftw3.h>
#endif

#ifndef M_LN2
  #define M_LN2   0.69314718055994530942  /* log_e 2 */
#endif


#define NUM_FFT_ALGOS  8
enum {
  ALGO_FFTPACK = 0,
  ALGO_VECLIB,
  ALGO_FFTW_ESTIM,
  ALGO_FFTW_AUTO,
  ALGO_GREEN,
  ALGO_KISS,
  ALGO_PFFFT_U, /* = 6 */
  ALGO_PFFFT_O  /* = 7 */
};

#define NUM_TYPES      7
enum {
  TYPE_PREP = 0,         /* time for preparation in ms */
  TYPE_DUR_NS = 1,       /* time per fft in ns */
  TYPE_DUR_FASTEST = 2,  /* relative time to fastest */
  TYPE_REL_PFFFT = 3,    /* relative time to ALGO_PFFFT */
  TYPE_ITER = 4,         /* # of iterations in measurement */
  TYPE_MFLOPS = 5,       /* MFlops/sec */
  TYPE_DUR_TOT = 6       /* test duration in sec */
};
/* double tmeas[NUM_TYPES][NUM_FFT_ALGOS]; */

const char * algoName[NUM_FFT_ALGOS] = {
  "FFTPack      ",
  "vDSP (vec)   ",
  "FFTW(estim)  ",
  "FFTW (auto)  ",
  "Green        ",
  "Kiss         ",
  "PFFFT-U(simd)",  /* unordered */
  "PFFFT (simd) "   /* ordered */
};


int compiledInAlgo[NUM_FFT_ALGOS] = {
  1, /* "FFTPack    " */
#ifdef HAVE_VECLIB
  1, /* "vDSP (vec) " */
#else
  0,
#endif
#ifdef HAVE_FFTW
  1, /* "FFTW(estim)" */
  1, /* "FFTW (auto)" */
#else
  0, 0,
#endif
#ifdef HAVE_GREEN_FFTS
  1, /* "Green      " */
#else
  0,
#endif
#ifdef HAVE_KISS_FFT
  1, /* "Kiss       " */
#else
  0,
#endif
  1, /* "PFFFT_U    " */
  1  /* "PFFFT_O    " */
};

const char * algoTableHeader[NUM_FFT_ALGOS][2] = {
{ "| real FFTPack ", "| cplx FFTPack " },
{ "|  real   vDSP ", "|  cplx   vDSP " },
{ "|real FFTWestim", "|cplx FFTWestim" },
{ "|real FFTWauto ", "|cplx FFTWauto " },
{ "|  real  Green ", "|  cplx  Green " },
{ "|  real   Kiss ", "|  cplx   Kiss " },
{ "| real PFFFT-U ", "| cplx PFFFT-U " },
{ "|  real  PFFFT ", "|  cplx  PFFFT " } };

const char * typeText[NUM_TYPES] = {
  "preparation in ms",
  "time per fft in ns",
  "relative to fastest",
  "relative to pffft",
  "measured_num_iters",
  "mflops",
  "test duration in sec"
};

const char * typeFilenamePart[NUM_TYPES] = {
  "1-preparation-in-ms",
  "2-timePerFft-in-ns",
  "3-rel-fastest",
  "4-rel-pffft",
  "5-num-iter",
  "6-mflops",
  "7-duration-in-sec"
};

#define SAVE_ALL_TYPES  0

const int saveType[NUM_TYPES] = {
  1, /* "1-preparation-in-ms" */
  0, /* "2-timePerFft-in-ns"  */
  0, /* "3-rel-fastest"       */
  1, /* "4-rel-pffft"         */
  1, /* "5-num-iter"          */
  1, /* "6-mflops"            */
  1, /* "7-duration-in-sec"   */
};


#define MAX(x,y) ((x)>(y)?(x):(y))
#define MIN(x,y) ((x)<(y)?(x):(y))

unsigned Log2(unsigned v) {
  /* we don't need speed records .. obvious way is good enough */
  /* https://graphics.stanford.edu/~seander/bithacks.html#IntegerLogObvious */
  /* Find the log base 2 of an integer with the MSB N set in O(N) operations (the obvious way):
   * unsigned v: 32-bit word to find the log base 2 of */
  unsigned r = 0; /* r will be lg(v) */
  while (v >>= 1)
  {
    r++;
  }
  return r;
}


double frand() {
  return rand()/(double)RAND_MAX;
}

#if defined(HAVE_SYS_TIMES)
  inline double uclock_sec(void) {
    static double ttclk = 0.;
    struct tms t;
    if (ttclk == 0.)
      ttclk = sysconf(_SC_CLK_TCK);
    times(&t);
    /* use only the user time of this process - not realtime, which depends on OS-scheduler .. */
    return ((double)t.tms_utime)) / ttclk;
  }
# else
  double uclock_sec(void)
{ return (double)clock()/(double)CLOCKS_PER_SEC; }
#endif


/* compare results with the regular fftpack */
void pffft_validate_N(int N, int cplx) {
  int Nfloat = N*(cplx?2:1);
  int Nbytes = Nfloat * sizeof(float);
  float *ref, *in, *out, *tmp, *tmp2;
  PFFFT_Setup *s = pffft_new_setup(N, cplx ? PFFFT_COMPLEX : PFFFT_REAL);
  int pass;

  if (!s) { printf("Skipping N=%d, not supported\n", N); return; }
  ref = pffft_aligned_malloc(Nbytes);
  in = pffft_aligned_malloc(Nbytes);
  out = pffft_aligned_malloc(Nbytes);
  tmp = pffft_aligned_malloc(Nbytes);
  tmp2 = pffft_aligned_malloc(Nbytes);

  for (pass=0; pass < 2; ++pass) {
    float ref_max = 0;
    int k;
    /* printf("N=%d pass=%d cplx=%d\n", N, pass, cplx); */
    /* compute reference solution with FFTPACK */
    if (pass == 0) {
      float *wrk = malloc(2*Nbytes+15*sizeof(float));
      for (k=0; k < Nfloat; ++k) {
        ref[k] = in[k] = frand()*2-1; 
        out[k] = 1e30;
      }
      if (!cplx) {
        rffti(N, wrk);
        rfftf(N, ref, wrk);
        /* use our ordering for real ffts instead of the one of fftpack */
        {
          float refN=ref[N-1];
          for (k=N-2; k >= 1; --k) ref[k+1] = ref[k]; 
          ref[1] = refN;
        }
      } else {
        cffti(N, wrk);
        cfftf(N, ref, wrk);
      }
      free(wrk);
    }

    for (k = 0; k < Nfloat; ++k) ref_max = MAX(ref_max, fabs(ref[k]));

      
    /* pass 0 : non canonical ordering of transform coefficients */
    if (pass == 0) {
      /* test forward transform, with different input / output */
      pffft_transform(s, in, tmp, 0, PFFFT_FORWARD);
      memcpy(tmp2, tmp, Nbytes);
      memcpy(tmp, in, Nbytes);
      pffft_transform(s, tmp, tmp, 0, PFFFT_FORWARD);
      for (k = 0; k < Nfloat; ++k) {
        assert(tmp2[k] == tmp[k]);
      }

      /* test reordering */
      pffft_zreorder(s, tmp, out, PFFFT_FORWARD);
      pffft_zreorder(s, out, tmp, PFFFT_BACKWARD);
      for (k = 0; k < Nfloat; ++k) {
        assert(tmp2[k] == tmp[k]);
      }
      pffft_zreorder(s, tmp, out, PFFFT_FORWARD);
    } else {
      /* pass 1 : canonical ordering of transform coeffs. */
      pffft_transform_ordered(s, in, tmp, 0, PFFFT_FORWARD);
      memcpy(tmp2, tmp, Nbytes);
      memcpy(tmp, in, Nbytes);
      pffft_transform_ordered(s, tmp, tmp, 0, PFFFT_FORWARD);
      for (k = 0; k < Nfloat; ++k) {
        assert(tmp2[k] == tmp[k]);
      }
      memcpy(out, tmp, Nbytes);
    }

    {
      for (k=0; k < Nfloat; ++k) {
        if (!(fabs(ref[k] - out[k]) < 1e-3*ref_max)) {
          printf("%s forward PFFFT mismatch found for N=%d\n", (cplx?"CPLX":"REAL"), N);
          exit(1);
        }
      }
        
      if (pass == 0) pffft_transform(s, tmp, out, 0, PFFFT_BACKWARD);
      else   pffft_transform_ordered(s, tmp, out, 0, PFFFT_BACKWARD);
      memcpy(tmp2, out, Nbytes);
      memcpy(out, tmp, Nbytes);
      if (pass == 0) pffft_transform(s, out, out, 0, PFFFT_BACKWARD);
      else   pffft_transform_ordered(s, out, out, 0, PFFFT_BACKWARD);
      for (k = 0; k < Nfloat; ++k) {
        assert(tmp2[k] == out[k]);
        out[k] *= 1.f/N;
      }
      for (k = 0; k < Nfloat; ++k) {
        if (fabs(in[k] - out[k]) > 1e-3 * ref_max) {
          printf("pass=%d, %s IFFFT does not match for N=%d\n", pass, (cplx?"CPLX":"REAL"), N); break;
          exit(1);
        }
      }
    }

    /* quick test of the circular convolution in fft domain */
    {
      float conv_err = 0, conv_max = 0;

      pffft_zreorder(s, ref, tmp, PFFFT_FORWARD);
      memset(out, 0, Nbytes);
      pffft_zconvolve_accumulate(s, ref, ref, out, 1.0);
      pffft_zreorder(s, out, tmp2, PFFFT_FORWARD);
      
      for (k=0; k < Nfloat; k += 2) {
        float ar = tmp[k], ai=tmp[k+1];
        if (cplx || k > 0) {
          tmp[k] = ar*ar - ai*ai;
          tmp[k+1] = 2*ar*ai;
        } else {
          tmp[0] = ar*ar;
          tmp[1] = ai*ai;
        }
      }
      
      for (k=0; k < Nfloat; ++k) {
        float d = fabs(tmp[k] - tmp2[k]), e = fabs(tmp[k]);
        if (d > conv_err) conv_err = d;
        if (e > conv_max) conv_max = e;
      }
      if (conv_err > 1e-5*conv_max) {
        printf("zconvolve error ? %g %g\n", conv_err, conv_max); exit(1);
      }
    }

  }

  printf("%s PFFFT is OK for N=%d\n", (cplx?"CPLX":"REAL"), N); fflush(stdout);
  
  pffft_destroy_setup(s);
  pffft_aligned_free(ref);
  pffft_aligned_free(in);
  pffft_aligned_free(out);
  pffft_aligned_free(tmp);
  pffft_aligned_free(tmp2);
}

void pffft_validate(int cplx) {
  static int Ntest[] = { 16, 32, 64, 96, 128, 160, 192, 256, 288, 384, 5*96, 512, 576, 5*128, 800, 864, 1024, 2048, 2592, 4000, 4096, 12000, 36864, 0};
  int k;
  for (k = 0; Ntest[k]; ++k) {
    int N = Ntest[k];
    if (N == 16 && !cplx) continue;
    pffft_validate_N(N, cplx);
  }
}

int array_output_format = 1;


void print_table(const char *txt, FILE *tableFile) {
  fprintf(stdout, "%s", txt);
  if (tableFile && tableFile != stdout)
    fprintf(tableFile, "%s", txt);
}

void print_table_flops(float mflops, FILE *tableFile) {
  fprintf(stdout, "|%11.0f   ", mflops);
  if (tableFile && tableFile != stdout)
    fprintf(tableFile, "|%11.0f   ", mflops);
}

void print_table_fftsize(int N, FILE *tableFile) {
  fprintf(stdout, "|%9d  ", N);
  if (tableFile && tableFile != stdout)
    fprintf(tableFile, "|%9d  ", N);
}

double show_output(const char *name, int N, int cplx, float flops, float t0, float t1, int max_iter, FILE *tableFile) {
  double T = (double)(t1-t0)/2/max_iter * 1e9;
  float mflops = flops/1e6/(t1 - t0 + 1e-16);
  if (array_output_format) {
    if (flops != -1)
      print_table_flops(mflops, tableFile);
    else
      print_table("|      n/a     ", tableFile);
  } else {
    if (flops != -1) {
      printf("N=%5d, %s %16s : %6.0f MFlops [t=%6.0f ns, %d runs]\n", N, (cplx?"CPLX":"REAL"), name, mflops, (t1-t0)/2/max_iter * 1e9, max_iter);
    }
  }
  fflush(stdout);
  return T;
}

void test_pffft_mem_align()
{
  int N, k;
  for ( N = 1; N < 4096; ++N ) {
    float * p0 = pffft_aligned_malloc( N * sizeof(float) );
    float * p = p0; /* pffft_aligned_addr(p0); */
    for ( k = 0; k < N; ++k )
      p[k] = k;
    pffft_aligned_free(p0);
  }
}


double cal_benchmark(int N, int cplx) {
  const int log2N = Log2(N);
  int Nfloat = (cplx ? N*2 : N);
  int Nbytes = Nfloat * sizeof(float);
  float *X = pffft_aligned_malloc(Nbytes), *Y = pffft_aligned_malloc(Nbytes), *Z = pffft_aligned_malloc(Nbytes);
  double t0, t1, tstop, T, nI;
  int k, iter;

  assert( pffft_is_power_of_two(N) );
  for (k = 0; k < Nfloat; ++k) {
    X[k] = sqrtf(k+1);
  }

  /* PFFFT-U (unordered) benchmark */
  PFFFT_Setup *s = pffft_new_setup(N, cplx ? PFFFT_COMPLEX : PFFFT_REAL);
  assert(s);
  iter = 0;
  t0 = uclock_sec();
  tstop = t0 + 0.25;  /* benchmark duration: 250 ms */
  do {
    for ( k = 0; k < 512; ++k ) {
      pffft_transform(s, X, Z, Y, PFFFT_FORWARD);
      pffft_transform(s, X, Z, Y, PFFFT_BACKWARD);
      ++iter;
    }
    t1 = uclock_sec();
  } while ( t1 < tstop );
  pffft_destroy_setup(s);
  pffft_aligned_free(X);
  pffft_aligned_free(Y);
  pffft_aligned_free(Z);

  T = ( t1 - t0 );  /* duration per fft() */
  nI = ((double)iter) * ( log2N * N );  /* number of iterations "normalized" to O(N) = N*log2(N) */
  return (nI / T);    /* normalized iterations per second */
}



void benchmark_ffts(int N, int cplx, int withFFTWfullMeas, double iterCal, double tmeas[NUM_TYPES][NUM_FFT_ALGOS], int haveAlgo[NUM_FFT_ALGOS], FILE *tableFile ) {
  const int log2N = Log2(N);
  int nextPow2N = pffft_next_power_of_two(N);
  int log2NextN = Log2(nextPow2N);
#ifdef PFFFT_SIMD_DISABLE
  int pffftPow2N = N;
#else
  int pffftPow2N = ( cplx ? ( MAX(N, 16) ) : ( MAX(N, 32) ) ); 
#endif

  int Nfloat = (cplx ? MAX(nextPow2N, pffftPow2N)*2 : MAX(nextPow2N, pffftPow2N));
  int Nmax, k, iter;
  int Nbytes = Nfloat * sizeof(float);

  float *X = pffft_aligned_malloc(Nbytes + sizeof(float)), *Y = pffft_aligned_malloc(Nbytes + 2*sizeof(float) ), *Z = pffft_aligned_malloc(Nbytes);
  double te, t0, t1, tstop, flops, Tfastest;

  const double max_test_duration = 0.150;   /* test duration 150 ms */
  double numIter = max_test_duration * iterCal / ( log2N * N );  /* number of iteration for max_test_duration */
  const int step_iter = MAX(1, ((int)(0.01 * numIter)) );  /* one hundredth */
  int max_iter = MAX(1, ((int)numIter) );  /* minimum 1 iteration */

  const float checkVal = 12345.0F;

  /* printf("benchmark_ffts(N = %d, cplx = %d): Nfloat = %d, X_mem = 0x%p, X = %p\n", N, cplx, Nfloat, X_mem, X); */

  memset( X, 0, Nfloat * sizeof(float) );
  if ( Nfloat < 32 ) {
    for (k = 0; k < Nfloat; k += 4)
      X[k] = sqrtf(k+1);
  } else {
    for (k = 0; k < Nfloat; k += (Nfloat/16) )
      X[k] = sqrtf(k+1);
  }

  for ( k = 0; k < NUM_TYPES; ++k )
  {
    for ( iter = 0; iter < NUM_FFT_ALGOS; ++iter )
      tmeas[k][iter] = 0.0;
  }


  /* FFTPack benchmark */
  Nmax = (cplx ? N*2 : N);
  X[Nmax] = checkVal;
  {
    float *wrk = malloc(2*Nbytes + 15*sizeof(float));
    te = uclock_sec();  
    if (cplx) cffti(N, wrk);
    else      rffti(N, wrk);
    t0 = uclock_sec();
    tstop = t0 + max_test_duration;
    max_iter = 0;
    do {
      for ( k = 0; k < step_iter; ++k ) {
        if (cplx) {
          assert( X[Nmax] == checkVal );
          cfftf(N, X, wrk);
          assert( X[Nmax] == checkVal );
          cfftb(N, X, wrk);
          assert( X[Nmax] == checkVal );
        } else {
          assert( X[Nmax] == checkVal );
          rfftf(N, X, wrk);
          assert( X[Nmax] == checkVal );
          rfftb(N, X, wrk);
          assert( X[Nmax] == checkVal );
        }
        ++max_iter;
      }
      t1 = uclock_sec();
    } while ( t1 < tstop );

    free(wrk);

    flops = (max_iter*2) * ((cplx ? 5 : 2.5)*N*log((double)N)/M_LN2); /* see http://www.fftw.org/speed/method.html */
    tmeas[TYPE_ITER][ALGO_FFTPACK] = max_iter;
    tmeas[TYPE_MFLOPS][ALGO_FFTPACK] = flops/1e6/(t1 - t0 + 1e-16);
    tmeas[TYPE_DUR_TOT][ALGO_FFTPACK] = t1 - t0;
    tmeas[TYPE_DUR_NS][ALGO_FFTPACK] = show_output("FFTPack", N, cplx, flops, t0, t1, max_iter, tableFile);
    tmeas[TYPE_PREP][ALGO_FFTPACK] = (t0 - te) * 1e3;
    haveAlgo[ALGO_FFTPACK] = 1;
  }

#ifdef HAVE_VECLIB
  Nmax = (cplx ? nextPow2N*2 : nextPow2N);
  X[Nmax] = checkVal;
  te = uclock_sec();
  if ( 1 || pffft_is_power_of_two(N) ) {
    FFTSetup setup;

    setup = vDSP_create_fftsetup(log2NextN, FFT_RADIX2);
    DSPSplitComplex zsamples;
    zsamples.realp = &X[0];
    zsamples.imagp = &X[Nfloat/2];
    t0 = uclock_sec();
    tstop = t0 + max_test_duration;
    max_iter = 0;
    do {
      for ( k = 0; k < step_iter; ++k ) {
        if (cplx) {
          assert( X[Nmax] == checkVal );
          vDSP_fft_zip(setup, &zsamples, 1, log2NextN, kFFTDirection_Forward);
          assert( X[Nmax] == checkVal );
          vDSP_fft_zip(setup, &zsamples, 1, log2NextN, kFFTDirection_Inverse);
          assert( X[Nmax] == checkVal );
        } else {
          assert( X[Nmax] == checkVal );
          vDSP_fft_zrip(setup, &zsamples, 1, log2NextN, kFFTDirection_Forward); 
          assert( X[Nmax] == checkVal );
          vDSP_fft_zrip(setup, &zsamples, 1, log2NextN, kFFTDirection_Inverse);
          assert( X[Nmax] == checkVal );
        }
        ++max_iter;
      }
      t1 = uclock_sec();
    } while ( t1 < tstop );

    vDSP_destroy_fftsetup(setup);
    flops = (max_iter*2) * ((cplx ? 5 : 2.5)*N*log((double)N)/M_LN2); /* see http://www.fftw.org/speed/method.html */
    tmeas[TYPE_ITER][ALGO_VECLIB] = max_iter;
    tmeas[TYPE_MFLOPS][ALGO_VECLIB] = flops/1e6/(t1 - t0 + 1e-16);
    tmeas[TYPE_DUR_TOT][ALGO_VECLIB] = t1 - t0;
    tmeas[TYPE_DUR_NS][ALGO_VECLIB] = show_output("vDSP", N, cplx, flops, t0, t1, max_iter, tableFile);
    tmeas[TYPE_PREP][ALGO_VECLIB] = (t0 - te) * 1e3;
    haveAlgo[ALGO_VECLIB] = 1;
  } else {
    show_output("vDSP", N, cplx, -1, -1, -1, -1, tableFile);
  }
#endif

#ifdef HAVE_FFTW
  Nmax = (cplx ? N*2 : N);
  X[Nmax] = checkVal;
  {
    /* int flags = (N <= (256*1024) ? FFTW_MEASURE : FFTW_ESTIMATE);  measure takes a lot of time on largest ffts */
    int flags = FFTW_ESTIMATE;
    te = uclock_sec();
    fftwf_plan planf, planb;
    fftw_complex *in = (fftw_complex*) fftwf_malloc(sizeof(fftw_complex) * N);
    fftw_complex *out = (fftw_complex*) fftwf_malloc(sizeof(fftw_complex) * N);
    memset(in, 0, sizeof(fftw_complex) * N);
    if (cplx) {
      planf = fftwf_plan_dft_1d(N, (fftwf_complex*)in, (fftwf_complex*)out, FFTW_FORWARD, flags);
      planb = fftwf_plan_dft_1d(N, (fftwf_complex*)in, (fftwf_complex*)out, FFTW_BACKWARD, flags);
    } else {
      planf = fftwf_plan_dft_r2c_1d(N, (float*)in, (fftwf_complex*)out, flags);
      planb = fftwf_plan_dft_c2r_1d(N, (fftwf_complex*)in, (float*)out, flags);
    }

    t0 = uclock_sec();
    tstop = t0 + max_test_duration;
    max_iter = 0;
    do {
      for ( k = 0; k < step_iter; ++k ) {
        assert( X[Nmax] == checkVal );
        fftwf_execute(planf);
        assert( X[Nmax] == checkVal );
        fftwf_execute(planb);
        assert( X[Nmax] == checkVal );
        ++max_iter;
      }
      t1 = uclock_sec();
    } while ( t1 < tstop );

    fftwf_destroy_plan(planf);
    fftwf_destroy_plan(planb);
    fftwf_free(in); fftwf_free(out);

    flops = (max_iter*2) * ((cplx ? 5 : 2.5)*N*log((double)N)/M_LN2); /* see http://www.fftw.org/speed/method.html */
    tmeas[TYPE_ITER][ALGO_FFTW_ESTIM] = max_iter;
    tmeas[TYPE_MFLOPS][ALGO_FFTW_ESTIM] = flops/1e6/(t1 - t0 + 1e-16);
    tmeas[TYPE_DUR_TOT][ALGO_FFTW_ESTIM] = t1 - t0;
    tmeas[TYPE_DUR_NS][ALGO_FFTW_ESTIM] = show_output((flags == FFTW_MEASURE ? algoName[ALGO_FFTW_AUTO] : algoName[ALGO_FFTW_ESTIM]), N, cplx, flops, t0, t1, max_iter, tableFile);
    tmeas[TYPE_PREP][ALGO_FFTW_ESTIM] = (t0 - te) * 1e3;
    haveAlgo[ALGO_FFTW_ESTIM] = 1;
  }
  Nmax = (cplx ? N*2 : N);
  X[Nmax] = checkVal;
  do {
    /* int flags = (N <= (256*1024) ? FFTW_MEASURE : FFTW_ESTIMATE);  measure takes a lot of time on largest ffts */
    /* int flags = FFTW_MEASURE; */
#if ( defined(__arm__) || defined(__aarch64__) || defined(__arm64__) )
    int limitFFTsize = 31;  /* takes over a second on Raspberry Pi 3 B+ -- and much much more on higher ffts sizes! */
#else
    int limitFFTsize = 2400;  /* take over a second on i7 for fft size 2400 */
#endif
    int flags = (N < limitFFTsize ? FFTW_MEASURE : (withFFTWfullMeas ? FFTW_MEASURE : FFTW_ESTIMATE));

    if (flags == FFTW_ESTIMATE) {
      show_output((flags == FFTW_MEASURE ? algoName[ALGO_FFTW_AUTO] : algoName[ALGO_FFTW_ESTIM]), N, cplx, -1, -1, -1, -1, tableFile);
      /* copy values from estimation */
      tmeas[TYPE_ITER][ALGO_FFTW_AUTO] = tmeas[TYPE_ITER][ALGO_FFTW_ESTIM];
      tmeas[TYPE_DUR_TOT][ALGO_FFTW_AUTO] = tmeas[TYPE_DUR_TOT][ALGO_FFTW_ESTIM];
      tmeas[TYPE_DUR_NS][ALGO_FFTW_AUTO] = tmeas[TYPE_DUR_NS][ALGO_FFTW_ESTIM];
      tmeas[TYPE_PREP][ALGO_FFTW_AUTO] = tmeas[TYPE_PREP][ALGO_FFTW_ESTIM];
    } else {
      te = uclock_sec();
      fftwf_plan planf, planb;
      fftw_complex *in = (fftw_complex*) fftwf_malloc(sizeof(fftw_complex) * N);
      fftw_complex *out = (fftw_complex*) fftwf_malloc(sizeof(fftw_complex) * N);
      memset(in, 0, sizeof(fftw_complex) * N);
      if (cplx) {
        planf = fftwf_plan_dft_1d(N, (fftwf_complex*)in, (fftwf_complex*)out, FFTW_FORWARD, flags);
        planb = fftwf_plan_dft_1d(N, (fftwf_complex*)in, (fftwf_complex*)out, FFTW_BACKWARD, flags);
      } else {
        planf = fftwf_plan_dft_r2c_1d(N, (float*)in, (fftwf_complex*)out, flags);
        planb = fftwf_plan_dft_c2r_1d(N, (fftwf_complex*)in, (float*)out, flags);
      }

      t0 = uclock_sec();
      tstop = t0 + max_test_duration;
      max_iter = 0;
      do {
        for ( k = 0; k < step_iter; ++k ) {
          assert( X[Nmax] == checkVal );
          fftwf_execute(planf);
          assert( X[Nmax] == checkVal );
          fftwf_execute(planb);
          assert( X[Nmax] == checkVal );
          ++max_iter;
        }
        t1 = uclock_sec();
      } while ( t1 < tstop );

      fftwf_destroy_plan(planf);
      fftwf_destroy_plan(planb);
      fftwf_free(in); fftwf_free(out);

      flops = (max_iter*2) * ((cplx ? 5 : 2.5)*N*log((double)N)/M_LN2); /* see http://www.fftw.org/speed/method.html */
      tmeas[TYPE_ITER][ALGO_FFTW_AUTO] = max_iter;
      tmeas[TYPE_MFLOPS][ALGO_FFTW_AUTO] = flops/1e6/(t1 - t0 + 1e-16);
      tmeas[TYPE_DUR_TOT][ALGO_FFTW_AUTO] = t1 - t0;
      tmeas[TYPE_DUR_NS][ALGO_FFTW_AUTO] = show_output((flags == FFTW_MEASURE ? algoName[ALGO_FFTW_AUTO] : algoName[ALGO_FFTW_ESTIM]), N, cplx, flops, t0, t1, max_iter, tableFile);
      tmeas[TYPE_PREP][ALGO_FFTW_AUTO] = (t0 - te) * 1e3;
      haveAlgo[ALGO_FFTW_AUTO] = 1;
    }
  } while (0);
#else
  (void)withFFTWfullMeas;
#endif  

#ifdef HAVE_GREEN_FFTS
  Nmax = (cplx ? nextPow2N*2 : nextPow2N);
  X[Nmax] = checkVal;
  if ( 1 || pffft_is_power_of_two(N) )
  {
    te = uclock_sec();
    fftInit(log2NextN);

    t0 = uclock_sec();
    tstop = t0 + max_test_duration;
    max_iter = 0;
    do {
      for ( k = 0; k < step_iter; ++k ) {
        if (cplx) {
          assert( X[Nmax] == checkVal );
          ffts(X, log2NextN, 1);
          assert( X[Nmax] == checkVal );
          iffts(X, log2NextN, 1);
          assert( X[Nmax] == checkVal );
        } else {
          rffts(X, log2NextN, 1);
          riffts(X, log2NextN, 1);
        }

        ++max_iter;
      }
      t1 = uclock_sec();
    } while ( t1 < tstop );

    fftFree();

    flops = (max_iter*2) * ((cplx ? 5 : 2.5)*N*log((double)N)/M_LN2); /* see http://www.fftw.org/speed/method.html */
    tmeas[TYPE_ITER][ALGO_GREEN] = max_iter;
    tmeas[TYPE_MFLOPS][ALGO_GREEN] = flops/1e6/(t1 - t0 + 1e-16);
    tmeas[TYPE_DUR_TOT][ALGO_GREEN] = t1 - t0;
    tmeas[TYPE_DUR_NS][ALGO_GREEN] = show_output("Green", N, cplx, flops, t0, t1, max_iter, tableFile);
    tmeas[TYPE_PREP][ALGO_GREEN] = (t0 - te) * 1e3;
    haveAlgo[ALGO_GREEN] = 1;
  } else {
    show_output("Green", N, cplx, -1, -1, -1, -1, tableFile);
  }
#endif

#ifdef HAVE_KISS_FFT
  Nmax = (cplx ? nextPow2N*2 : nextPow2N);
  X[Nmax] = checkVal;
  if ( 1 || pffft_is_power_of_two(N) )
  {
    kiss_fft_cfg stf;
    kiss_fft_cfg sti;
    kiss_fftr_cfg stfr;
    kiss_fftr_cfg stir;

    te = uclock_sec();
    if (cplx) {
      stf = kiss_fft_alloc(nextPow2N, 0, 0, 0);
      sti = kiss_fft_alloc(nextPow2N, 1, 0, 0);
    } else {
      stfr = kiss_fftr_alloc(nextPow2N, 0, 0, 0);
      stir = kiss_fftr_alloc(nextPow2N, 1, 0, 0);
    }

    t0 = uclock_sec();
    tstop = t0 + max_test_duration;
    max_iter = 0;
    do {
      for ( k = 0; k < step_iter; ++k ) {
        if (cplx) {
          assert( X[Nmax] == checkVal );
          kiss_fft(stf, (const kiss_fft_cpx *)X, (kiss_fft_cpx *)Y);
          assert( X[Nmax] == checkVal );
          kiss_fft(sti, (const kiss_fft_cpx *)Y, (kiss_fft_cpx *)X);
          assert( X[Nmax] == checkVal );
        } else {
          assert( X[Nmax] == checkVal );
          kiss_fftr(stfr, X, (kiss_fft_cpx *)Y);
          assert( X[Nmax] == checkVal );
          kiss_fftri(stir, (const kiss_fft_cpx *)Y, X);
          assert( X[Nmax] == checkVal );
        }
        ++max_iter;
      }
      t1 = uclock_sec();
    } while ( t1 < tstop );

    kiss_fft_cleanup();

    flops = (max_iter*2) * ((cplx ? 5 : 2.5)*N*log((double)N)/M_LN2); /* see http://www.fftw.org/speed/method.html */
    tmeas[TYPE_ITER][ALGO_KISS] = max_iter;
    tmeas[TYPE_MFLOPS][ALGO_KISS] = flops/1e6/(t1 - t0 + 1e-16);
    tmeas[TYPE_DUR_TOT][ALGO_KISS] = t1 - t0;
    tmeas[TYPE_DUR_NS][ALGO_KISS] = show_output("Kiss", N, cplx, flops, t0, t1, max_iter, tableFile);
    tmeas[TYPE_PREP][ALGO_KISS] = (t0 - te) * 1e3;
    haveAlgo[ALGO_KISS] = 1;
  } else {
    show_output("Kiss", N, cplx, -1, -1, -1, -1, tableFile);
  }
#endif


  /* PFFFT-U (unordered) benchmark */
  Nmax = (cplx ? pffftPow2N*2 : pffftPow2N);
  X[Nmax] = checkVal;
  {
    te = uclock_sec();
    PFFFT_Setup *s = pffft_new_setup(pffftPow2N, cplx ? PFFFT_COMPLEX : PFFFT_REAL);
    if (s) {
      t0 = uclock_sec();
      tstop = t0 + max_test_duration;
      max_iter = 0;
      do {
        for ( k = 0; k < step_iter; ++k ) {
          assert( X[Nmax] == checkVal );
          pffft_transform(s, X, Z, Y, PFFFT_FORWARD);
          assert( X[Nmax] == checkVal );
          pffft_transform(s, X, Z, Y, PFFFT_BACKWARD);
          assert( X[Nmax] == checkVal );
          ++max_iter;
        }
        t1 = uclock_sec();
      } while ( t1 < tstop );

      pffft_destroy_setup(s);

      flops = (max_iter*2) * ((cplx ? 5 : 2.5)*N*log((double)N)/M_LN2); /* see http://www.fftw.org/speed/method.html */
      tmeas[TYPE_ITER][ALGO_PFFFT_U] = max_iter;
      tmeas[TYPE_MFLOPS][ALGO_PFFFT_U] = flops/1e6/(t1 - t0 + 1e-16);
      tmeas[TYPE_DUR_TOT][ALGO_PFFFT_U] = t1 - t0;
      tmeas[TYPE_DUR_NS][ALGO_PFFFT_U] = show_output("PFFFT-U", N, cplx, flops, t0, t1, max_iter, tableFile);
      tmeas[TYPE_PREP][ALGO_PFFFT_U] = (t0 - te) * 1e3;
      haveAlgo[ALGO_PFFFT_U] = 1;
    }
  }
  {
    te = uclock_sec();
    PFFFT_Setup *s = pffft_new_setup(pffftPow2N, cplx ? PFFFT_COMPLEX : PFFFT_REAL);
    if (s) {
      t0 = uclock_sec();
      tstop = t0 + max_test_duration;
      max_iter = 0;
      do {
        for ( k = 0; k < step_iter; ++k ) {
          assert( X[Nmax] == checkVal );
          pffft_transform_ordered(s, X, Z, Y, PFFFT_FORWARD);
          assert( X[Nmax] == checkVal );
          pffft_transform_ordered(s, X, Z, Y, PFFFT_BACKWARD);
          assert( X[Nmax] == checkVal );
          ++max_iter;
        }
        t1 = uclock_sec();
      } while ( t1 < tstop );

      pffft_destroy_setup(s);

      flops = (max_iter*2) * ((cplx ? 5 : 2.5)*N*log((double)N)/M_LN2); /* see http://www.fftw.org/speed/method.html */
      tmeas[TYPE_ITER][ALGO_PFFFT_O] = max_iter;
      tmeas[TYPE_MFLOPS][ALGO_PFFFT_O] = flops/1e6/(t1 - t0 + 1e-16);
      tmeas[TYPE_DUR_TOT][ALGO_PFFFT_O] = t1 - t0;
      tmeas[TYPE_DUR_NS][ALGO_PFFFT_O] = show_output("PFFFT", N, cplx, flops, t0, t1, max_iter, tableFile);
      tmeas[TYPE_PREP][ALGO_PFFFT_O] = (t0 - te) * 1e3;
      haveAlgo[ALGO_PFFFT_O] = 1;
    }
  }


  if (!array_output_format)
  {
    printf("prepare/ms:     ");
    for ( iter = 0; iter < NUM_FFT_ALGOS; ++iter )
    {
      if ( haveAlgo[iter] && tmeas[TYPE_DUR_NS][iter] > 0.0 ) {
        printf("%s %.3f    ", algoName[iter], tmeas[TYPE_PREP][iter] );
      }
    }
    printf("\n");
  }
  Tfastest = 0.0;
  for ( iter = 0; iter < NUM_FFT_ALGOS; ++iter )
  {
    if ( Tfastest == 0.0 || ( tmeas[TYPE_DUR_NS][iter] != 0.0 && tmeas[TYPE_DUR_NS][iter] < Tfastest ) )
      Tfastest = tmeas[TYPE_DUR_NS][iter];
  }
  if ( Tfastest > 0.0 )
  {
    if (!array_output_format)
      printf("relative fast:  ");
    for ( iter = 0; iter < NUM_FFT_ALGOS; ++iter )
    {
      if ( haveAlgo[iter] && tmeas[TYPE_DUR_NS][iter] > 0.0 ) {
        tmeas[TYPE_DUR_FASTEST][iter] = tmeas[TYPE_DUR_NS][iter] / Tfastest;
        if (!array_output_format)
          printf("%s %.3f    ", algoName[iter], tmeas[TYPE_DUR_FASTEST][iter] );
      }
    }
    if (!array_output_format)
      printf("\n");
  }

  {
    if (!array_output_format)
      printf("relative pffft: ");
    for ( iter = 0; iter < NUM_FFT_ALGOS; ++iter )
    {
      if ( haveAlgo[iter] && tmeas[TYPE_DUR_NS][iter] > 0.0 ) {
        tmeas[TYPE_REL_PFFFT][iter] = tmeas[TYPE_DUR_NS][iter] / tmeas[TYPE_DUR_NS][ALGO_PFFFT_O];
        if (!array_output_format)
          printf("%s %.3f    ", algoName[iter], tmeas[TYPE_REL_PFFFT][iter] );
      }
    }
    if (!array_output_format)
      printf("\n");
  }

  if (!array_output_format) {
    printf("--\n");
  }

  pffft_aligned_free(X);
  pffft_aligned_free(Y);
  pffft_aligned_free(Z);
}

#ifndef PFFFT_SIMD_DISABLE
void validate_pffft_simd(); /* a small function inside pffft.c that will detect compiler bugs with respect to simd instruction */
#endif



int main(int argc, char **argv) {
  /* unfortunately, the fft size must be a multiple of 16 for complex FFTs 
     and 32 for real FFTs -- a lot of stuff would need to be rewritten to
     handle other cases (or maybe just switch to a scalar fft, I don't know..) */

#if 0  /* include powers of 2 ? */
#define NUMNONPOW2LENS  23
  int NnonPow2[NUMNONPOW2LENS] = {
    64, 96, 128, 160, 192,   256, 384, 5*96, 512, 5*128,
    3*256, 800, 1024, 2048, 2400,   4096, 8192, 9*1024, 16384, 32768,
    256*1024, 1024*1024, -1 };
#else
#define NUMNONPOW2LENS  11
  int NnonPow2[NUMNONPOW2LENS] = {
    96, 160, 192, 384, 5*96,   5*128,3*256, 800, 2400, 9*1024,
    -1 };
#endif

#define NUMPOW2FFTLENS  21
#define MAXNUMFFTLENS MAX( NUMPOW2FFTLENS, NUMNONPOW2LENS )
  int Npow2[NUMPOW2FFTLENS];  /* exp = 1 .. 20, -1 */
  const int *Nvalues = NULL;
  double tmeas[2][MAXNUMFFTLENS][NUM_TYPES][NUM_FFT_ALGOS];
  double iterCalReal, iterCalCplx;

  int benchReal=1, benchCplx=1, withFFTWfullMeas=0, outputTable2File=1, usePow2=1;
  int realCplxIdx, typeIdx;
  int i, k;
  int smallestCplxN = pffft_simd_size()*pffft_simd_size();
  int smallestRealN = 2*smallestCplxN;
  FILE *tableFile = NULL;

  int haveAlgo[NUM_FFT_ALGOS];
  char acCsvFilename[32];

  for ( k = 1; k <= NUMPOW2FFTLENS; ++k )
    Npow2[k-1] = (k == NUMPOW2FFTLENS) ? -1 : (1 << k);
  Nvalues = Npow2;  /* set default .. for comparisons .. */

  for ( i = 0; i < NUM_FFT_ALGOS; ++i )
    haveAlgo[i] = 0;

  for ( i = 1; i < argc; ++i ) {
    if (!strcmp(argv[i], "--array-format") || !strcmp(argv[i], "--table")) {
      array_output_format = 1;
    }
    else if (!strcmp(argv[i], "--no-tab")) {
      array_output_format = 0;
    }
    else if (!strcmp(argv[i], "--real")) {
      benchCplx = 0;
    }
    else if (!strcmp(argv[i], "--cplx")) {
      benchReal = 0;
    }
    else if (!strcmp(argv[i], "--fftw-full-measure")) {
      withFFTWfullMeas = 1;
    }
    else if (!strcmp(argv[i], "--non-pow2")) {
      Nvalues = NnonPow2;
      usePow2 = 0;
    }
    else /* if (!strcmp(argv[i], "--help")) */ {
      printf("usage: %s [--array-format|--table] [--no-tab] [--real|--cplx] [--fftw-full-measure] [--non-pow2]\n", argv[0]);
      exit(0);
    }
  }

#ifdef HAVE_FFTW
  if (withFFTWfullMeas)
  {
    algoName[ALGO_FFTW_AUTO] = "FFTW(meas.)"; /* "FFTW (auto)" */
    algoTableHeader[NUM_FFT_ALGOS][0] = "|real FFTWmeas "; /* "|real FFTWauto " */
    algoTableHeader[NUM_FFT_ALGOS][0] = "|cplx FFTWmeas "; /* "|cplx FFTWauto " */
  }
#endif

#ifdef PFFFT_SIMD_DISABLE
  algoName[ALGO_PFFFT_U] = "PFFFT_U(scal)";
#else
  validate_pffft_simd();
#endif
  pffft_validate(1);
  pffft_validate(0);
  test_pffft_mem_align();

  clock();
  /* double TClockDur = 1.0 / CLOCKS_PER_SEC;
  printf("clock() duration for CLOCKS_PER_SEC = %f sec = %f ms\n", TClockDur, 1000.0 * TClockDur );
  */

  /* calibrate test duration */
  {
    double t0, t1, dur;
    printf("calibrating fft benchmark duration at size N = 512 ..\n");
    t0 = uclock_sec();
    if (benchReal) {
      iterCalReal = cal_benchmark(512, 0 /* real fft */);
      printf("real fft iterCal = %f\n", iterCalReal);
    }
    if (benchCplx) {
      iterCalCplx = cal_benchmark(512, 1 /* cplx fft */);
      printf("cplx fft iterCal = %f\n", iterCalCplx);
    }
    t1 = uclock_sec();
    dur = t1 - t0;
    printf("calibration done in %f sec.\n", dur);
  }

  if (!array_output_format) {
    if (benchReal) {
      for (i=0; Nvalues[i] > 0; ++i)
        benchmark_ffts(Nvalues[i], 0 /* real fft */, withFFTWfullMeas, iterCalReal, tmeas[0][i], haveAlgo, NULL);
    }
    if (benchCplx) {
      for (i=0; Nvalues[i] > 0; ++i)
        benchmark_ffts(Nvalues[i], 1 /* cplx fft */, withFFTWfullMeas, iterCalCplx, tmeas[1][i], haveAlgo, NULL);
    }

  } else {

    if (outputTable2File) {
      tableFile = fopen( usePow2 ? "bench-fft-table-pow2.txt" : "bench-fft-table-non2.txt", "w");
    }
    /* print table headers */
    {
      print_table("| input len ", tableFile);
      for (realCplxIdx = 0; realCplxIdx < 2; ++realCplxIdx)
      {
        if ( (realCplxIdx == 0 && !benchReal) || (realCplxIdx == 1 && !benchCplx) )
          continue;
        for (k=0; k < NUM_FFT_ALGOS; ++k)
        {
          if ( compiledInAlgo[k] )
            print_table(algoTableHeader[k][realCplxIdx], tableFile);
        }
      }
      print_table("|\n", tableFile);
    }
    /* print table value seperators */
    {
      print_table("|----------", tableFile);
      for (realCplxIdx = 0; realCplxIdx < 2; ++realCplxIdx)
      {
        if ( (realCplxIdx == 0 && !benchReal) || (realCplxIdx == 1 && !benchCplx) )
          continue;
        for (k=0; k < NUM_FFT_ALGOS; ++k)
        {
          if ( compiledInAlgo[k] )
            print_table(":|-------------", tableFile);
        }
      }
      print_table(":|\n", tableFile);
    }

    for (i=0; Nvalues[i] > 0; ++i) {
      /* if ( Nvalues[i] >= smallestRealN && Nvalues[i] >= smallestCplxN ) */
      {
        double t0, t1;
        print_table_fftsize(Nvalues[i], tableFile);
        t0 = uclock_sec();
        if (benchReal)
          benchmark_ffts(Nvalues[i], 0, withFFTWfullMeas, iterCalReal, tmeas[0][i], haveAlgo, tableFile);
        if (benchCplx)
          benchmark_ffts(Nvalues[i], 1, withFFTWfullMeas, iterCalCplx, tmeas[1][i], haveAlgo, tableFile);
        t1 = uclock_sec();
        print_table("|\n", tableFile);
        /* printf("all ffts for size %d took %f sec\n", Nvalues[i], t1-t0); */
        (void)t0;
        (void)t1;
      }
    }
    fprintf(stdout, " (numbers are given in MFlops)\n");
    if (outputTable2File) {
      fclose(tableFile);
    }
  }


  printf("\n\n");
  printf("smallest cplx fft size: %d\n", smallestCplxN);
  printf("smallest real fft size: %d\n", smallestRealN);

  printf("\n");
  printf("now writing .csv files ..\n");

  for (realCplxIdx = 0; realCplxIdx < 2; ++realCplxIdx)
  {
    if ( (benchReal && realCplxIdx == 0) || (benchCplx && realCplxIdx == 1) )
    {
      for (typeIdx = 0; typeIdx < NUM_TYPES; ++typeIdx)
      {
        FILE *f = NULL;
        if ( !(SAVE_ALL_TYPES || saveType[typeIdx]) )
          continue;
        acCsvFilename[0] = 0;
#ifdef PFFFT_SIMD_DISABLE
        strcat(acCsvFilename, "scal-");
#else
        strcat(acCsvFilename, "simd-");
#endif
        strcat(acCsvFilename, (realCplxIdx == 0 ? "real-" : "cplx-"));
        strcat(acCsvFilename, ( usePow2 ? "pow2-" : "non2-"));
        strcat(acCsvFilename, typeFilenamePart[typeIdx]);
        strcat(acCsvFilename, ".csv");
        f = fopen(acCsvFilename, "w");
        if (!f)
          continue;
        {
          fprintf(f, "size, log2, ");
          for (k=0; k < NUM_FFT_ALGOS; ++k)
            if ( haveAlgo[k] )
              fprintf(f, "%s, ", algoName[k]);
          fprintf(f, "\n");
        }
        for (i=0; Nvalues[i] > 0; ++i)
        {
          if ( (benchReal && Nvalues[i] >= smallestRealN) || (benchCplx && Nvalues[i] >= smallestCplxN) )
          {
            fprintf(f, "%d, %.3f, ", Nvalues[i], log10((double)Nvalues[i])/log10(2.0) );
            for (k=0; k < NUM_FFT_ALGOS; ++k)
              if ( haveAlgo[k] )
                fprintf(f, "%f, ", tmeas[realCplxIdx][i][typeIdx][k]);
            fprintf(f, "\n");
          }
        }
        fclose(f);
      }
    }
  }

  return 0;
}

