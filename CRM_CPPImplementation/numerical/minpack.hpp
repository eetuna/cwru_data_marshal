#pragma once
/*
	Modified from C++ version of the MINPACK library by John Burkardt.
	https://people.sc.fsu.edu/~jburkardt/cpp_src/minpack/minpack.html
	Original FORTRAN77 version by Jorge More, Danny Sorenson, Burton Garbow, Kenneth Hillstrom
		Jorge More, Burton Garbow, Kenneth Hillstrom,
		User Guide for MINPACK-1,
		Technical Report ANL-80-74,
		Argonne National Laboratory, 1980.
		Available at: https://www.osti.gov/biblio/6997568
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


//#include "CRM.hpp"

template <typename ParamType>
using NLEquationType = void (*) (double in_x[], double out_y[], ParamType Params);

template <typename ParamType>
using NLEquationJacobianType = void (*) (double in_x[], double out_y[], double out_fjac[], ParamType Params);

template <typename ParamType>
void TrustRegionDogleg(NLEquationType<ParamType> NLEquation,
	int n, double x[], double fvec[], double tol, int& info, ParamType Params);

template <typename ParamType>
void TrustRegionDogleg(	NLEquationType<ParamType> NLEquation,
						int n, double x[], double fvec[], double tol, int& info, double wa[], int lwa, ParamType Params);

template <typename ParamType>
void hybrd(		NLEquationType<ParamType> NLEquation,
				int n, double x[], double fvec[], double xtol, int maxfev, int ml, int mu, double epsfcn,
						double diag[], int mode, double factor, int nprint, int& info, int& nfev,
						double fjac[], int ldfjac, double r[], int lr, double qtf[],
						double wa1[], double wa2[], double wa3[], double wa4[], ParamType Params);

template <typename ParamType>
void TrustRegionDogleg_GivenJacobian(NLEquationType<ParamType> NLEquation, NLEquationJacobianType<ParamType> NLEquationJac,
	int n, double x[], double fvec[], double tol, int& info, ParamType Params);

template <typename ParamType>
void TrustRegionDogleg_GivenJacobian(NLEquationType<ParamType> NLEquation, NLEquationJacobianType<ParamType> NLEquationJac,
	int n, double x[], double fvec[], double tol, int& info, double wa[], int lwa, ParamType Params);

template <typename ParamType>
void hybrj(NLEquationType<ParamType> NLEquation, NLEquationJacobianType<ParamType> NLEquationJac,
	int n, double x[], double fvec[], double xtol, int maxfev,
	double diag[], int mode, double factor, int nprint, int& info, int& nfev, int& njev,
	double fjac[], int ldfjac, double r[], int lr, double qtf[],
	double wa1[], double wa2[], double wa3[], double wa4[], ParamType Params);

void dogleg(	int n, double r[], int lr, double diag[], double qtb[], double delta, double x[],
						double wa1[], double wa2[]);

template <typename ParamType>
void fdjac1(	NLEquationType<ParamType> NLEquation,
				int n, double x[], double fvec[], double fjac[], int ldfjac, int& iflag,
						int ml, int mu, double epsfcn, double wa1[], double wa2[], ParamType Params);

double enorm(	int n, double x[]);

void qform(		int m, int n, double q[], int ldq, double wa[]);

void qrfac(		int m, int n, double a[], int lda, bool pivot, int ipvt[], int lipvt,
							double rdiag[], double acnorm[], double wa[]);

void r1mpyq(	int m, int n, double a[], int lda, double v[], double w[]);

void r1updt(	int m, int n, double s[], int ls, double u[], double v[], double w[], bool& sing);


#include "minpack_TemplateDefinitions.hpp"
#include "minpack_wGivenJac_TemplateDefinitions.hpp"