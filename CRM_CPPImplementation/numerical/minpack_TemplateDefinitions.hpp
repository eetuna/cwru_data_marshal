#pragma once
/*
    Original FORTRAN77 version by Jorge More, Danny Sorenson, Burton Garbow, Kenneth Hillstrom
        Jorge More, Burton Garbow, Kenneth Hillstrom,
        User Guide for MINPACK-1,
        Technical Report ANL-80-74,
        Argonne National Laboratory, 1980.
        Available at: https://www.osti.gov/biblio/6997568
	The FORTRAN77 version of the code was converted to C++ using fable fortran to c++ converter
		https://cci.lbl.gov/fable/
		and was than manually edited to eliminate superfluous libraries, and to conform to C++ array indexing.
		I have only converted the functions needed to be able to use hybrd1 solver.
	I have used the C++ version of the MINPACK library by John Burkardt for checking correctness.
		https://people.sc.fsu.edu/~jburkardt/cpp_src/minpack/minpack.html
	Note that care should be taken if a parallelization of the algorithm will be pursued due to the use of a common
		work array wa, which is crated once outside the hybrd1 function and reused in all of the subroutines being
		executed inside hybrd1.  This current version is more efficient as it does not require subsequent/repeated
		memory allocations within each subroutine for working memory.  However, it may lead to conflicts if things
		are parallelized within subroutines.  (Note that, the version by Burkhardt allocates some work arrays inside
		the subroutines.)

		------
    Minpack Copyright Notice(1999) University of Chicago.All rights reserved

    Redistributionand use in sourceand binary forms, with or
    without modification, are permitted provided that the
    following conditions are met :

    1. Redistributions of source code must retain the above
    copyright notice, this list of conditionsand the following
    disclaimer.

    2. Redistributions in binary form must reproduce the above
    copyright notice, this list of conditionsand the following
    disclaimer in the documentationand /or other materials
    provided with the distribution.

    3. The end - user documentation included with the
    redistribution, if any, must include the following
    acknowledgment :

    "This product includes software developed by the
    University of Chicago, as Operator of Argonne National
    Laboratory.

    Alternately, this acknowledgment may appear in the software
    itself, ifand wherever such third - party acknowledgments
    normally appear.

    4. WARRANTY DISCLAIMER.THE SOFTWARE IS SUPPLIED "AS IS"
    WITHOUT WARRANTY OF ANY KIND.THE COPYRIGHT HOLDER, THE
    UNITED STATES, THE UNITED STATES DEPARTMENT OF ENERGY, AND
    THEIR EMPLOYEES : (1) DISCLAIM ANY WARRANTIES, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO ANY IMPLIED WARRANTIES
    OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, TITLE
    OR NON - INFRINGEMENT, (2) DO NOT ASSUME ANY LEGAL LIABILITY
    OR RESPONSIBILITY FOR THE ACCURACY, COMPLETENESS, OR
    USEFULNESS OF THE SOFTWARE, (3) DO NOT REPRESENT THAT USE OF
    THE SOFTWARE WOULD NOT INFRINGE PRIVATELY OWNED RIGHTS, (4)
    DO NOT WARRANT THAT THE SOFTWARE WILL FUNCTION
    UNINTERRUPTED, THAT IT IS ERROR - FREE OR THAT ANY ERRORS WILL
    BE CORRECTED.

    5. LIMITATION OF LIABILITY.IN NO EVENT WILL THE COPYRIGHT
    HOLDER, THE UNITED STATES, THE UNITED STATES DEPARTMENT OF
    ENERGY, OR THEIR EMPLOYEES : BE LIABLE FOR ANY INDIRECT,
    INCIDENTAL, CONSEQUENTIAL, SPECIAL OR PUNITIVE DAMAGES OF
    ANY KIND OR NATURE, INCLUDING BUT NOT LIMITED TO LOSS OF
    PROFITS OR LOSS OF DATA, FOR ANY REASON WHATSOEVER, WHETHER
    SUCH LIABILITY IS ASSERTED ON THE BASIS OF CONTRACT, TORT
    (INCLUDING NEGLIGENCE OR STRICT LIABILITY), OR OTHERWISE,
    EVEN IF ANY OF SAID PARTIES HAS BEEN WARNED OF THE
    POSSIBILITY OF SUCH LOSS OR DAMAGES.
    */

#include <cmath>

#define MAX(a,b) 	( ((b)>(a))?(b):(a) )
#define MIN(a,b) 	( ((b)<(a))?(b):(a) )
#define SQR(X) 		( (X)*(X) )
#define FABS(x)		( ((x)>=0)?(x):(-x) )

//C     Function dpmpar
//C
//C     Double precision machine parameters
//C         If the machine has
//C         t base b digits and its smallest and largest exponents are
//C         emin and emax, respectively, then these parameters are
//C
//C         dpmpar(1) = b**(1 - t), the machine precision,
//C
//C         dpmpar(2) = b**(emin - 1), the smallest magnitude,
//C
//C         dpmpar(3) = b**emax*(1 - b**(-t)), the largest magnitude.
//C
//C     Argonne National Laboratory. MINPACK Project. November 1996.
//C     Burton S. Garbow, Kenneth E. Hillstrom, Jorge J. More'
#define DPMPAR1 2.220446049250313e-16
//#define DPMPAR2 2.22507385852e-308
//#define DPMPAR3 1.79769313485e+308
#define DPMPAR2 0.4450147717014e-307
#define DPMPAR3 1.0e+30

template <typename ParamType>
void TrustRegionDogleg(NLEquationType<ParamType> NLEquation,
	int n, double x[], double fvec[], double tol, int& info, ParamType Params) {
	//
	//  TrustRegionDogleg() with a simpler interface
	//     Buffers are created inside this wrapper function, rather then being created and passed by the caller
	//
	int lwa = (n * (3 * n + 13)) / 2;
	double* wa = new double[lwa];

	TrustRegionDogleg(NLEquation, n, x, fvec, tol, info, wa, lwa, Params);

	delete[] wa;
}


template <typename ParamType>
void TrustRegionDogleg(	NLEquationType<ParamType> NLEquation,
						int n, double x[], double fvec[], double tol, int& info, double wa[], int lwa, ParamType Params) {
	//C     **********
	//C
	//C     subroutine hybrd1
	//C
	//C     the purpose of hybrd1 is to find a zero of a system of
	//C     n nonlinear functions in n variables by a modification
	//C     of the powell hybrid method. this is done by using the
	//C     more general nonlinear equation solver hybrd. the user
	//C     must provide a subroutine which calculates the functions.
	//C     the jacobian is then calculated by a forward-difference
	//C     approximation.
	//C
	//C     the subroutine statement is
	//C
	//C       subroutine hybrd1(fcn,n,x,fvec,tol,info,wa,lwa)
	//C
	//C     where
	//C
	//C       fcn is the name of the user-supplied subroutine which    /// fcn() is replaced with hard coded NLEquation()
	//C         calculates the functions. fcn must be declared
	//C         in an external statement in the user calling
	//C         program, and should be written as follows.
	//C
	//C         subroutine fcn(n,x,fvec,iflag)
	//C         integer n,iflag
	//C         double precision x(n),fvec(n)
	//C         ----------
	//C         calculate the functions at x and
	//C         return this vector in fvec.
	//C         ---------
	//C         return
	//C         end
	//C
	//C         the value of iflag should not be changed by fcn unless
	//C         the user wants to terminate execution of hybrd1.
	//C         in this case set iflag to a negative integer.
	//C
	//C       n is a positive integer input variable set to the number
	//C         of functions and variables.
	//C
	//C       x is an array of length n. on input x must contain
	//C         an initial estimate of the solution vector. on output x
	//C         contains the final estimate of the solution vector.
	//C
	//C       fvec is an output array of length n which contains
	//C         the functions evaluated at the output x.
	//C
	//C       tol is a nonnegative input variable. termination occurs
	//C         when the algorithm estimates that the relative error
	//C         between x and the solution is at most tol.
	//C
	//C       info is an integer output variable. if the user has
	//C         terminated execution, info is set to the (negative)
	//C         value of iflag. see description of fcn. otherwise,
	//C         info is set as follows.
	//C
	//C         info = 0   improper input parameters.
	//C
	//C         info = 1   algorithm estimates that the relative error
	//C                    between x and the solution is at most tol.
	//C
	//C         info = 2   number of calls to fcn has reached or exceeded
	//C                    200*(n+1).
	//C
	//C         info = 3   tol is too small. no further improvement in
	//C                    the approximate solution x is possible.
	//C
	//C         info = 4   iteration is not making good progress.
	//C
	//C       wa is a work array of length lwa.
	//C
	//C       lwa is a positive integer input variable not less than
	//C         (n*(3*n+13))/2.
	//C
	//C     subprograms called
	//C
	//C       user-supplied ...... fcn
	//C
	//C       minpack-supplied ... hybrd
	//C
	//C     argonne national laboratory. minpack project. march 1980.
	//C     burton s. garbow, kenneth e. hillstrom, jorge j. more
	//C
	//C     **********
	double factor = 100.0;
	int maxfev = 200 * (n + 1);
	double xtol = tol;
	int ml = n - 1;
	int mu = n - 1;
	double epsfcn = 0.0;
	int mode = 2;
	int j;
	int nprint = 0;
	int lr = (n * (n + 1)) / 2;
	int index = 6 * n + lr;;
	int nfev;	
	info = 0;
	//C
	//C     check the input parameters for errors.
	//C
	if (n <= 0 || tol < 0.0 || lwa < (n * (3 * n + 13)) / 2) {
		return;
	}
	//C
	//C     call hybrd.
	//C
	for (j = 0; j < n; j++) {
		wa[j] = 1.0;
	}
	hybrd(NLEquation, n, x, fvec, xtol, maxfev, ml, mu, epsfcn, wa,
		mode, factor, nprint, info, nfev, wa+index, n, wa+6*n,
		lr, wa+n, wa+2*n, wa+3*n, wa+4*n, wa+5*n, Params);
	if (info == 5) {
		info = 4;
	}
}


template <typename ParamType>
void hybrd(NLEquationType<ParamType> NLEquation,
	int n, double x[], double fvec[], double xtol, int maxfev, int ml, int mu, double epsfcn,
	double diag[], int mode, double factor, int nprint, int& info, int& nfev,
	double fjac[], int ldfjac, double r[], int lr, double qtf[],
	double wa1[], double wa2[], double wa3[], double wa4[], ParamType Params) {
	//C     **********
	//C
	//C     subroutine hybrd
	//C
	//C     the purpose of hybrd is to find a zero of a system of
	//C     n nonlinear functions in n variables by a modification
	//C     of the powell hybrid method. the user must provide a
	//C     subroutine which calculates the functions. the jacobian is
	//C     then calculated by a forward-difference approximation.
	//C
	//C     the subroutine statement is
	//C
	//C       subroutine hybrd(fcn,n,x,fvec,xtol,maxfev,ml,mu,epsfcn,
	//C                        diag,mode,factor,nprint,info,nfev,fjac,
	//C                        ldfjac,r,lr,qtf,wa1,wa2,wa3,wa4)
	//C
	//C     where
	//C
	//C       fcn is the name of the user-supplied subroutine which
	//C         calculates the functions. fcn must be declared
	//C         in an external statement in the user calling
	//C         program, and should be written as follows.
	//C
	//C         subroutine fcn(n,x,fvec,iflag)
	//C         integer n,iflag
	//C         double precision x(n),fvec(n)
	//C         ----------
	//C         calculate the functions at x and
	//C         return this vector in fvec.
	//C         ---------
	//C         return
	//C         end
	//C
	//C         the value of iflag should not be changed by fcn unless
	//C         the user wants to terminate execution of hybrd.
	//C         in this case set iflag to a negative integer.
	//C
	//C       n is a positive integer input variable set to the number
	//C         of functions and variables.
	//C
	//C       x is an array of length n. on input x must contain
	//C         an initial estimate of the solution vector. on output x
	//C         contains the final estimate of the solution vector.
	//C
	//C       fvec is an output array of length n which contains
	//C         the functions evaluated at the output x.
	//C
	//C       xtol is a nonnegative input variable. termination
	//C         occurs when the relative error between two consecutive
	//C         iterates is at most xtol.
	//C
	//C       maxfev is a positive integer input variable. termination
	//C         occurs when the number of calls to fcn is at least maxfev
	//C         by the end of an iteration.
	//C
	//C       ml is a nonnegative integer input variable which specifies
	//C         the number of subdiagonals within the band of the
	//C         jacobian matrix. if the jacobian is not banded, set
	//C         ml to at least n - 1.
	//C
	//C       mu is a nonnegative integer input variable which specifies
	//C         the number of superdiagonals within the band of the
	//C         jacobian matrix. if the jacobian is not banded, set
	//C         mu to at least n - 1.
	//C
	//C       epsfcn is an input variable used in determining a suitable
	//C         step length for the forward-difference approximation. this
	//C         approximation assumes that the relative errors in the
	//C         functions are of the order of epsfcn. if epsfcn is less
	//C         than the machine precision, it is assumed that the relative
	//C         errors in the functions are of the order of the machine
	//C         precision.
	//C
	//C       diag is an array of length n. if mode = 1 (see
	//C         below), diag is internally set. if mode = 2, diag
	//C         must contain positive entries that serve as
	//C         multiplicative scale factors for the variables.
	//C
	//C       mode is an integer input variable. if mode = 1, the
	//C         variables will be scaled internally. if mode = 2,
	//C         the scaling is specified by the input diag. other
	//C         values of mode are equivalent to mode = 1.
	//C
	//C       factor is a positive input variable used in determining the
	//C         initial step bound. this bound is set to the product of
	//C         factor and the euclidean norm of diag*x if nonzero, or else
	//C         to factor itself. in most cases factor should lie in the
	//C         interval (.1,100.). 100. is a generally recommended value.
	//C
	//C       nprint is an integer input variable that enables controlled
	//C         printing of iterates if it is positive. in this case,
	//C         fcn is called with iflag = 0 at the beginning of the first
	//C         iteration and every nprint iterations thereafter and
	//C         immediately prior to return, with x and fvec available
	//C         for printing. if nprint is not positive, no special calls
	//C         of fcn with iflag = 0 are made.
	//C
	//C       info is an integer output variable. if the user has
	//C         terminated execution, info is set to the (negative)
	//C         value of iflag. see description of fcn. otherwise,
	//C         info is set as follows.
	//C
	//C         info = 0   improper input parameters.
	//C
	//C         info = 1   relative error between two consecutive iterates
	//C                    is at most xtol.
	//C
	//C         info = 2   number of calls to fcn has reached or exceeded
	//C                    maxfev.
	//C
	//C         info = 3   xtol is too small. no further improvement in
	//C                    the approximate solution x is possible.
	//C
	//C         info = 4   iteration is not making good progress, as
	//C                    measured by the improvement from the last
	//C                    five jacobian evaluations.
	//C
	//C         info = 5   iteration is not making good progress, as
	//C                    measured by the improvement from the last
	//C                    ten iterations.
	//C
	//C       nfev is an integer output variable set to the number of
	//C         calls to fcn.
	//C
	//C       fjac is an output n by n array which contains the
	//C         orthogonal matrix q produced by the qr factorization
	//C         of the final approximate jacobian.
	//C
	//C       ldfjac is a positive integer input variable not less than n
	//C         which specifies the leading dimension of the array fjac.
	//C
	//C       r is an output array of length lr which contains the
	//C         upper triangular matrix produced by the qr factorization
	//C         of the final approximate jacobian, stored rowwise.
	//C
	//C       lr is a positive integer input variable not less than
	//C         (n*(n+1))/2.
	//C
	//C       qtf is an output array of length n which contains
	//C         the vector (q transpose)*fvec.
	//C
	//C       wa1, wa2, wa3, and wa4 are work arrays of length n.
	//C
	//C     subprograms called
	//C
	//C       user-supplied ...... fcn
	//C
	//C       minpack-supplied ... dogleg,dpmpar,enorm,fdjac1,
	//C                            qform,qrfac,r1mpyq,r1updt
	//C
	//C       fortran-supplied ... dabs,dmax1,dmin1,min0,mod
	//C
	//C     argonne national laboratory. minpack project. march 1980.
	//C     burton s. garbow, kenneth e. hillstrom, jorge j. more
	//C
	//C     **********
	const double p0001 = 0.0001;
	const double p001 = 0.001;
	const double p1 = 0.1;
	const double p5 = 0.5;
	int iflag = 0;
	int j;
	double fnorm;
	int msum;
	int iter;
	int ncsuc;
	int ncfail;
	int nslow1;
	int nslow2;
	bool jeval;
	int iwa[1];
	double xnorm;
	double delta;
	int i;
	double sum;
	double temp;
	bool sing;
	int l;
	double pnorm;
	double fnorm1;
	double actred;
	double prered;
	double ratio;
	//C
	//C     epsmch is the machine precision.
	//C
	const double epsmch = DPMPAR1;
	//C
	info = 0;
	nfev = 0;
	//C
	//C     check the input parameters for errors.
	//C
	if (n <= 0 || xtol < 0.0 || maxfev <= 0 || ml < 0 || mu < 0 ||
		factor <= 0.0 || ldfjac < n || lr < (n * (n + 1)) / 2) {
		return;
	}
	if (mode == 2) {
		for (j = 0; j < n; j++) {
			if (diag[j] <= 0.0) {
				info = 0;
				return;
			}
		}
	}
	//C
	//C     evaluate the function at the starting point
	//C     and calculate its norm.
	//C
	iflag = 1;
	NLEquation(x, fvec, Params);
	nfev = 1;
	if (iflag < 0) {
		info = iflag;
		return;
	}
	fnorm = enorm(n, fvec);
	//C
	//C     determine the number of calls to fcn needed to compute
	//C     the jacobian matrix.
	//C
	msum = MIN(ml + mu + 1, n);
	//C
	//C     initialize iteration counter and monitors.
	//C
	iter = 1;
	ncsuc = 0;
	ncfail = 0;
	nslow1 = 0;
	nslow2 = 0;
	//C
	//C     beginning of the outer loop.
	//C
	while (1) {
		jeval = true;
		//C
		//C        calculate the jacobian matrix.
		//C
		iflag = 2;
		fdjac1(NLEquation, n, x, fvec, fjac, ldfjac, iflag, ml, mu, epsfcn, wa1, wa2, Params);
		nfev += msum;
		if (iflag < 0) {
			info = iflag;
			return;
		}
		//C
		//C        compute the qr factorization of the jacobian.
		//C
		qrfac(n, n, fjac, ldfjac, false, iwa, 1, wa1, wa2, wa3);
		//C
		//C        on the first iteration and if mode is 1, scale according
		//C        to the norms of the columns of the initial jacobian.
		//C
		if (iter == 1) {
			if (mode != 2) {
				for (j = 0; j < n; j++) {
					if (wa2[j] == 0.0) {
						diag[j] = 1.0;
					}
					else {
						diag[j] = wa2[j];
					}
				}
			}			//C
			//C        on the first iteration, calculate the norm of the scaled x
			//C        and initialize the step bound delta.
			//C
			for (j = 0; j < n; j++) {
				wa3[j] = diag[j] * x[j];
			}
			xnorm = enorm(n, wa3);
			delta = factor * xnorm;
			if (delta == 0.0) {
				delta = factor;
			}
		}
		//C
		//C        form (q transpose)*fvec and store in qtf.
		//C
		for (i = 0; i < n; i++) {
			qtf[i] = fvec[i];
		}
		for (j = 0; j < n; j++) {
			if (fjac[j + j * ldfjac] != 0.0) {
				sum = 0.0;
				for (i = j; i < n; i++) {
					sum += fjac[i + j * ldfjac] * qtf[i];
				}
				temp = -sum / fjac[j + j * ldfjac];
				for (i = j; i < n; i++) {
					qtf[i] += fjac[i + j * ldfjac] * temp;
				}
			}
		}
		//C
		//C        copy the triangular factor of the qr factorization into r.
		//C
		sing = false;
		for (j = 1; j <= n; j++) {
			l = j;
			for (i = 1; i <= j - 1; i++) {
				r[l - 1] = fjac[(i - 1) + (j - 1) * ldfjac];
				l += n - i;
			}
			r[l - 1] = wa1[j - 1];
			if (wa1[j - 1] == 0.0) {
				sing = true;
			}
		}
		//C
		//C        accumulate the orthogonal factor in fjac.
		//C
		qform(n, n, fjac, ldfjac, wa1);
		//C
		//C        rescale if necessary.
		//C
		if (mode != 2) {
			for (j = 0; j < n; j++) {
				diag[j] = MAX(diag[j], wa2[j]);
			}
		}
		//C
		//C        beginning of the inner loop.
		//C
		while (1) {
			//C
			//C           if requested, call fcn to enable printing of iterates.
			//C
			if (nprint > 0) {
				iflag = 0;
				if (((iter - 1) % nprint) == 0) {
					NLEquation(x, fvec, Params);
				}
				if (iflag < 0) {
					info = iflag;
					return;
				}
			}
			//C
			//C           determine the direction p.
			//C
			dogleg(n, r, lr, diag, qtf, delta, wa1, wa2, wa3);
			//C
			//C           store the direction p and x + p. calculate the norm of p.
			//C
			for (j = 0; j < n; j++) {
				wa1[j] = -wa1[j];
				wa2[j] = x[j] + wa1[j];
				wa3[j] = diag[j] * wa1[j];
			}
			pnorm = enorm(n, wa3);
			//C
			//C           on the first iteration, adjust the initial step bound.
			//C
			if (iter == 1) {
				delta = MIN(delta, pnorm);
			}
			//C
			//C           evaluate the function at x + p and calculate its norm.
			//C
			iflag = 1;
			NLEquation(wa2, wa4, Params);
			nfev++;
			if (iflag < 0) {
				info = iflag;
				return;
			}
			fnorm1 = enorm(n, wa4);
			//C
			//C           compute the scaled actual reduction.
			//C
			actred = -1.0;
			if (fnorm1 < fnorm) {
				actred = 1.0 - SQR(fnorm1 / fnorm);
			}
			//C
			//C           compute the scaled predicted reduction.
			//C
			l = 1;
			for (i = 1; i <= n; i++) {
				sum = 0.0;
				for (j = i; j <= n; j++) {
					sum += r[l - 1] * wa1[j - 1];
					l++;
				}
				wa3[i - 1] = qtf[i - 1] + sum;
			}
			temp = enorm(n, wa3);
			prered = 0.0;
			if (temp < fnorm) {
				prered = 1.0 - SQR(temp / fnorm);
			}
			//C
			//C           compute the ratio of the actual to the predicted
			//C           reduction.
			//C
			ratio = 0.0;
			if (prered > 0.0) {
				ratio = actred / prered;
			}
			//C
			//C           update the step bound.
			//C
			if (ratio < p1) {
				ncsuc = 0;
				ncfail++;
				delta = p5 * delta;
			}
			else {
				ncfail = 0;
				ncsuc++;
				if (ratio >= p5 || ncsuc > 1) {
					delta = MAX(delta, pnorm / p5);
				}
				if (FABS(ratio - 1.0) <= p1) {
					delta = pnorm / p5;
				}
			}
			//C
			//C           test for successful iteration.
			//C
			if (ratio >= p0001) {
				//C
				//C           successful iteration. update x, fvec, and their norms.
				//C
				for (j = 0; j < n; j++) {
					x[j] = wa2[j];
					wa2[j] = diag[j] * x[j];
					fvec[j] = wa4[j];
				}
				xnorm = enorm(n, wa2);
				fnorm = fnorm1;
				iter++;
			}
			//C
			//C           determine the progress of the iteration.
			//C
			nslow1++;
			if (actred >= p001) {
				nslow1 = 0;
			}
			if (jeval) {
				nslow2++;
			}
			if (actred >= p1) {
				nslow2 = 0;
			}
			//C
			//C           test for convergence.
			//C
			if (delta <= xtol * xnorm || fnorm == 0.0) {
				info = 1;
				return;
			}
			//C
			//C           tests for termination and stringent tolerances.
			//C
			if (nfev >= maxfev) {
				info = 2;
				return;
			}
			if (p1 * MAX(p1 * delta, pnorm) <= epsmch * xnorm) {
				info = 3;
				return;
			}
			if (nslow2 == 5) {
				info = 4;
				return;
			}
			if (nslow1 == 10) {
				info = 5;
				return;
			}
			//C
			//C           criterion for recalculating jacobian approximation
			//C           by forward differences.
			//C
			if (ncfail == 2) {
				break;
			}
			//C
			//C           calculate the rank one modification to the jacobian
			//C           and update qtf if necessary.
			//C
			for (j = 0; j < n; j++) {
				sum = 0.0;
				for (i = 0; i < n; i++) {
					sum += fjac[i + j * ldfjac] * wa4[i];
				}
				wa2[j] = (sum - wa3[j]) / pnorm;
				wa1[j] = diag[j] * ((diag[j] * wa1[j]) / pnorm);
				if (ratio >= p0001) {
					qtf[j] = sum;
				}
			}
			//C
			//C           compute the qr factorization of the updated jacobian.
			//C
			r1updt(n, n, r, lr, wa1, wa2, wa3, sing);
			r1mpyq(n, n, fjac, ldfjac, wa2, wa3);
			r1mpyq(1, n, qtf, 1, wa2, wa3);
			//C
			//C           end of the inner loop.
			//C
			jeval = false;
		}
		//C
		//C        end of the outer loop.
		//C
	}
	//C
	//C     termination, either normal or user imposed.
	//C
	if (iflag < 0) {
		info = iflag;
	}
	iflag = 0;
	if (nprint > 0) {
		NLEquation(x, fvec, Params);
	}
	//C
	//C     last card of subroutine hybrd.
	//C
}


template <typename ParamType>
void fdjac1(NLEquationType<ParamType> NLEquation,
	int n, double x[], double fvec[], double fjac[], int ldfjac, int& iflag,
	int ml, int mu, double epsfcn, double wa1[], double wa2[], ParamType Params) {
	//C     **********
	//C
	//C     subroutine fdjac1
	//C
	//C     this subroutine computes a forward-difference approximation
	//C     to the n by n jacobian matrix associated with a specified
	//C     problem of n functions in n variables. if the jacobian has
	//C     a banded form, then function evaluations are saved by only
	//C     approximating the nonzero terms.
	//C
	//C     the subroutine statement is
	//C
	//C       subroutine fdjac1(fcn,n,x,fvec,fjac,ldfjac,iflag,ml,mu,epsfcn,
	//C                         wa1,wa2)
	//C
	//C     where
	//C
	//C       fcn is the name of the user-supplied subroutine which
	//C         calculates the functions. fcn must be declared
	//C         in an external statement in the user calling
	//C         program, and should be written as follows.
	//C
	//C         subroutine fcn(n,x,fvec,iflag)
	//C         integer n,iflag
	//C         double precision x(n),fvec(n)
	//C         ----------
	//C         calculate the functions at x and
	//C         return this vector in fvec.
	//C         ----------
	//C         return
	//C         end
	//C
	//C         the value of iflag should not be changed by fcn unless
	//C         the user wants to terminate execution of fdjac1.
	//C         in this case set iflag to a negative integer.
	//C
	//C       n is a positive integer input variable set to the number
	//C         of functions and variables.
	//C
	//C       x is an input array of length n.
	//C
	//C       fvec is an input array of length n which must contain the
	//C         functions evaluated at x.
	//C
	//C       fjac is an output n by n array which contains the
	//C         approximation to the jacobian matrix evaluated at x.
	//C
	//C       ldfjac is a positive integer input variable not less than n
	//C         which specifies the leading dimension of the array fjac.
	//C
	//C       iflag is an integer variable which can be used to terminate
	//C         the execution of fdjac1. see description of fcn.
	//C
	//C       ml is a nonnegative integer input variable which specifies
	//C         the number of subdiagonals within the band of the
	//C         jacobian matrix. if the jacobian is not banded, set
	//C         ml to at least n - 1.
	//C
	//C       epsfcn is an input variable used in determining a suitable
	//C         step length for the forward-difference approximation. this
	//C         approximation assumes that the relative errors in the
	//C         functions are of the order of epsfcn. if epsfcn is less
	//C         than the machine precision, it is assumed that the relative
	//C         errors in the functions are of the order of the machine
	//C         precision.
	//C
	//C       mu is a nonnegative integer input variable which specifies
	//C         the number of superdiagonals within the band of the
	//C         jacobian matrix. if the jacobian is not banded, set
	//C         mu to at least n - 1.
	//C
	//C       wa1 and wa2 are work arrays of length n. if ml + mu + 1 is at
	//C         least n, then the jacobian is considered dense, and wa2 is
	//C         not referenced.
	//C
	//C     subprograms called
	//C
	//C       minpack-supplied ... dpmpar
	//C
	//C       fortran-supplied ... dabs,dmax1,dsqrt
	//C
	//C     argonne national laboratory. minpack project. march 1980.
	//C     burton s. garbow, kenneth e. hillstrom, jorge j. more
	//C
	//C     **********
	//C
	//C     epsmch is the machine precision.
	//C
	const double epsmch = DPMPAR1;
	double eps;
	double temp;
	double h;
	int msum;
	int j;
	int i;
	int k;
	//C
	eps = sqrt(MAX(epsfcn, epsmch));
	msum = ml + mu + 1;
	if (msum >= n) {
		//C
		//C        computation of dense approximate jacobian.
		//C
		for (j = 0; j < n; j++) {
			temp = x[j];
			h = eps * FABS(temp);
			if (h == 0.0) {
				h = eps;
			}
			x[j] = temp + h;
			NLEquation(x, wa1, Params);
			if (iflag < 0) {
				return;
			}
			x[j] = temp;
			for (i = 0; i < n; i++) {
				fjac[i + j * ldfjac] = (wa1[i] - fvec[i]) / h;
			}
		}
	}
	else {
		//C
		//C        computation of banded approximate jacobian.
		//C
		for (k = 0; k < msum; k++) {
			for (j = k; j < n; j+=msum) {
				wa2[j] = x[j];
				h = eps * FABS(wa2[j]);
				if (h == 0.0) {
					h = eps;
				}
				x[j] = wa2[j] + h;
			}
			NLEquation(x, wa1, Params);
			if (iflag < 0) {
				return;
			}
			for (j = k; j < n; j+=msum) {
				x[j] = wa2[j];
				h = eps * FABS(wa2[j]);
				if (h == 0.0) {
					h = eps;
				}
				for (i = 0; i < n; i++) {
					if (i >= j - mu && i <= j + ml) {
						fjac[i + j * ldfjac] = (wa1[i] - fvec[i]) / h;
					}
					else {
						fjac[i + j * ldfjac] = 0.0;
					}
				}
			}
		}
	}
	//C
	//C     last card of subroutine fdjac1.
	//C
}

