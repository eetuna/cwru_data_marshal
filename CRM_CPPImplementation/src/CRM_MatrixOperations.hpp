#pragma once

namespace CRMCatheterModel {

	// all matrices are assumed to be stored as a one dimensional array in row major order, unless otherwise noted

	//matrix multiplication C=A*B  ( A:D1xD2 B:D2xD3 gives C:D1xD3 )
	template <int D1, int D2, int D3>
	void mMult_AB(const double in_A[D1 * D2], const double in_B[D2 * D3], double out_C[D1 * D3]) {

		for (int i = 0; i < D1; i++) {
			for (int j = 0; j < D3; j++) {
				double sum = 0.0;
				for (int k = 0; k < D2; k++) {
					sum += in_A[i * D2 + k] * in_B[k * D3 + j];
				}
				out_C[i * D3 + j] = sum;
			}
		}

	}


	//matrix multiplication C=A^T*B  (A transposed times B)  ( A:D1xD2, A^T:D2xD1, B:D1xD3 gives C:D2xD3 )
	template <int D1, int D2, int D3>
	void mMult_ATB(const double in_A[D1 * D2], const double in_B[D1 * D3], double out_C[D2 * D3]) {

		for (int i = 0; i < D2; i++) {
			for (int j = 0; j < D3; j++) {
				double sum = 0.0;
				for (int k = 0; k < D1; k++) {
					sum += in_A[k * D2 + i] * in_B[k * D3 + j];
				}
				out_C[i * D3 + j] = sum;
			}
		}

	}


	//matrix multiplication C=A*B^T  (A times B transposed)  ( A:D1xD2, B:D3xD2, B^T:D2xD3,  gives C:D1xD3 )
	template <int D1, int D2, int D3>
	void mMult_ABT(const double in_A[D1 * D2], const double in_B[D3 * D2], double out_C[D1 * D3]) {

		for (int i = 0; i < D1; i++) {
			for (int j = 0; j < D3; j++) {
				double sum = 0.0;
				for (int k = 0; k < D2; k++) {
					sum += in_A[i * D2 + k] * in_B[j * D2 + k];
				}
				out_C[i * D3 + j] = sum;
			}
		}

	}


	// matrix scalar multiplication C=s*A  ( s:scalar, A,C:D1xD2 )
	template <int D1, int D2>
	void mMult_sA(const double s, const double in_A[D1 * D2], double out_C[D1 * D2]) {

		for (int i = 0; i < D1; i++) {
			for (int j = 0; j < D2; j++) {
				out_C[i * D2 + j] = s * in_A[i * D2 + j];
			}
		}

	}


	//matrix multiply and add C=C+A*B  ( A:D1xD2 B:D2xD3 gives C:D1xD3 )
	template <int D1, int D2, int D3>
	void mMultAdd_AB(const double in_A[D1 * D2], const double in_B[D2 * D3], double out_C[D1 * D3]) {

		for (int i = 0; i < D1; i++) {
			for (int j = 0; j < D3; j++) {
				double sum = 0.0;
				for (int k = 0; k < D2; k++) {
					sum += in_A[i * D2 + k] * in_B[k * D3 + j];
				}
				out_C[i * D3 + j] += sum;
			}
		}

	}


	//matrix multiply and subtract C=C-A*B  ( A:D1xD2 B:D2xD3 gives C:D1xD3 )
	template <int D1, int D2, int D3>
	void mMultSub_AB(const double in_A[D1 * D2], const double in_B[D2 * D3], double out_C[D1 * D3]) {

		for (int i = 0; i < D1; i++) {
			for (int j = 0; j < D3; j++) {
				double sum = 0.0;
				for (int k = 0; k < D2; k++) {
					sum += in_A[i * D2 + k] * in_B[k * D3 + j];
				}
				out_C[i * D3 + j] -= sum;
			}
		}

	}


	// matrix addition X=A+B, A=A+B, X=A+sB,  or X=A+B+C   ( A,B,C,X:D1xD2, s:scalar )
	template <int D1, int D2>
	void mAdd_AB(const double in_A[D1 * D2], const double in_B[D1 * D2], double out_X[D1 * D2]) {

		for (int i = 0; i < D1; i++) {
			for (int j = 0; j < D2; j++) {
				out_X[i * D2 + j] = in_A[i * D2 + j] + in_B[i * D2 + j];
			}
		}

	}

	template <int D1, int D2>
	void mAdd_AB(double inout_A[D1 * D2], const double in_B[D1 * D2]) {

		for (int i = 0; i < D1; i++) {
			for (int j = 0; j < D2; j++) {
				inout_A[i * D2 + j] += in_B[i * D2 + j];
			}
		}

	}

	template <int D1, int D2>
	void mAdd_AsB(const double in_A[D1 * D2], const double in_s, const double in_B[D1 * D2], double out_X[D1 * D2]) {

		for (int i = 0; i < D1; i++) {
			for (int j = 0; j < D2; j++) {
				out_X[i * D2 + j] = in_A[i * D2 + j] + in_s * in_B[i * D2 + j];
			}
		}

	}

	template <int D1, int D2>
	void mAdd_ABC(const double in_A[D1 * D2], const double in_B[D1 * D2], const double in_C[D1 * D2], double out_X[D1 * D2]) {

		for (int i = 0; i < D1; i++) {
			for (int j = 0; j < D2; j++) {
				out_X[i * D2 + j] = in_A[i * D2 + j] + in_B[i * D2 + j] + in_C[i * D2 + j];
			}
		}

	}


	// matrix subtraction X=A-B   ( A,B,X:D1xD2 )
	template <int D1, int D2>
	void mSub_AB(const double in_A[D1 * D2], const double in_B[D1 * D2], double out_X[D1 * D2]) {

		for (int i = 0; i < D1; i++) {
			for (int j = 0; j < D2; j++) {
				out_X[i * D2 + j] = in_A[i * D2 + j] - in_B[i * D2 + j];
			}
		}

	}


	// matrix subtraction A=A-B   ( A,B:D1xD2 )
	template <int D1, int D2>
	void mSub_AB(double inout_A[D1 * D2], const double in_B[D1 * D2]) {

		for (int i = 0; i < D1; i++) {
			for (int j = 0; j < D2; j++) {
				inout_A[i * D2 + j] -= in_B[i * D2 + j];
			}
		}

	}


	// 2-norm of a vector  ( D1x1 vector )
	template <int D1>
	double vNormSq(const double v[D1]) {

		double vn = 0.0;

		for (int i = 0; i < D1; i++) {
			vn += v[i] * v[i];
		}
		return vn;

	}


	//copy matrix: B=A  (D1xD2 matrices)
	template <int D1, int D2>
	void mCopy_ABm(const double in_A[D1][D2], double out_B[D1][D2]) {

		for (int i = 0; i < D1; i++) {
			for (int j = 0; j < D2; j++) {
				out_B[i][j] = in_A[i][j];
			}
		}

	}



	//copy vector: B=A  (D1x1 vector)
	template <int D1>
	void mCopy_AB(const double in_A[D1], double out_B[D1]) {

		for (int i = 0; i < D1; i++) {
			out_B[i] = in_A[i];
		}

	}

}
