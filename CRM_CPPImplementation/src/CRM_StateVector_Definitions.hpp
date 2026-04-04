#pragma once
#include <assert.h>
//#include <typeinfo>

namespace CRMCatheterModel {

	constexpr unsigned int CURRENT_ACT_VECTOR_DIM = NUM_ACT_SET * 3;
	constexpr unsigned int ACT_VECTOR_DIM = CURRENT_ACT_VECTOR_DIM + 1;
	constexpr unsigned int TIP_FORCE_DIM = 3;
	constexpr unsigned int SIMPLE_STATE_VECTOR_SIZE = (3 + 9 + 3);		// p, R, u
#ifdef ANALYTICAL_SE3_STEP
	constexpr unsigned int SIMPLE_STATE_DERIVATIVE_VECTOR_SIZE = SIMPLE_STATE_VECTOR_SIZE - (3 + 9);  // exclude p and R
#else
	constexpr unsigned int SIMPLE_STATE_DERIVATIVE_VECTOR_SIZE = SIMPLE_STATE_VECTOR_SIZE;  // include p and R
#endif
//constexpr unsigned int IVP_JACOBIAN_STATE_SIZE = (3 + 3 + 3) * (3 + 3 + 3 + NUM_ACT_SET * 3 + 1 + TIP_FORCE_DIM);  // (_p, ws, _u) x (p0, w0, u0, zc, zl, ftip) 
	constexpr unsigned int IVP_JACOBIAN_FULL_STATE_SIZE = (3 + 3 + 3) * (3 + NUM_ACT_SET * 3 + 1 + TIP_FORCE_DIM);  // (_p, ws, _u) x (u0, zc, zl, ftip) 
	constexpr unsigned int IVP_JACOBIAN_MINI_STATE_SIZE = (3 + 3 + 3) * (3);  // (_p, ws, _u) x (u0) 


	// type definition for mapping raw arrays to Eigen matrices
	//   -- note that, by convention, we have been using row major order for matrices
	template<int Rows, int Cols>
	using __EMT = Eigen::Map<Eigen::Matrix<double, Rows, Cols, Eigen::RowMajor> >;

	template<int Rows>
	using __EVT = Eigen::Map<Eigen::Matrix<double, Rows, 1> >;



	template <unsigned int N>
	class StateBase {
	public:
		StateBase() : _dim(N) {};
		StateBase(const StateBase<N>& obj) : _dim(N) {  // copy constructor - it is a must have if we have dynamic memory allocation
			for (unsigned int i = 0; i < N; i++) data[i] = obj.data[i];
		};
		virtual ~StateBase() {};

		//double& operator[](const unsigned int idx) { assert(idx < N);  return data[idx]; };
		//const double& operator[](const unsigned int idx) const { assert(idx < N);  return data[idx]; };
		virtual const StateBase<N> operator+(const StateBase<N>& obj) const {
			StateBase<N> result;
			for (unsigned int i = 0; i < N; i++) result.data[i] = this->data[i] + obj.data[i];
			return result;
		};
		const StateBase<N> operator*(const double& scalar) {
			StateBase<N> result;
			for (unsigned int i = 0; i < N; i++) result.data[i] = this->data[i] * scalar;
			return result;
		};
		friend const StateBase<N> operator*(const double& scalar, const StateBase<N>& obj) {
			StateBase<N> result;
			for (unsigned int i = 0; i < N; i++) result.data[i] = scalar * obj.data[i];
			return result;
		};

		double absmax() {
			double mx = 0.0;
			for (unsigned int i = 0; i < _dim; i++) mx = MAX(mx, abs(data[i]));
			return mx;
		};

		friend std::ostream& operator<<(std::ostream& os, const StateBase<N>& obj) {
			for (unsigned int i = 0; i < obj._dim - 1; i++) os << obj.data[i] << ", ";
			os << obj.data[obj._dim - 1] << "\n";
			return os;
		};

	protected:
		double data[N] = { };		// component array, initialized to zero
		unsigned int _dim;

	private:

	};

#ifdef ANALYTICAL_SE3_STEP
#define __INIT() _u(this->data)
#else
#define __INIT() _p(this->data), _R(this->data+3), _u(this->data+3+9)
#endif

	class StateDerivativeVector : public StateBase<SIMPLE_STATE_DERIVATIVE_VECTOR_SIZE> {
	public:
		StateDerivativeVector() : StateBase<SIMPLE_STATE_DERIVATIVE_VECTOR_SIZE>(), __INIT() { };
		StateDerivativeVector(const StateDerivativeVector& obj) :	// copy constructor
			StateBase<SIMPLE_STATE_DERIVATIVE_VECTOR_SIZE>(obj), __INIT() { };
		StateDerivativeVector(const StateBase<SIMPLE_STATE_DERIVATIVE_VECTOR_SIZE>& obj) :
			StateBase<SIMPLE_STATE_DERIVATIVE_VECTOR_SIZE>(obj), __INIT() { };
		virtual ~StateDerivativeVector() {};

		virtual StateDerivativeVector& operator=(const StateDerivativeVector& rhs) {
			if (this == &rhs)  return *this;		// If same object skip assignment, and just return *this.
			for (unsigned int i = 0; i < this->_dim; i++) this->data[i] = rhs.data[i];
			return *this;
		};

#ifndef ANALYTICAL_SE3_STEP
		double* const _p;
		double* const _R;
#endif
		double* const _u;

	protected:

	private:

	};

#undef __INIT


#define __INIT() _p(this->data), _R(this->data + 3), _u(this->data + 3 + 9)

	class StateVector : public StateBase<SIMPLE_STATE_VECTOR_SIZE> {
	public:
		StateVector() : StateBase<SIMPLE_STATE_VECTOR_SIZE>(), __INIT() { };
		StateVector(const StateVector& obj) :						// copy constructor
			StateBase<SIMPLE_STATE_VECTOR_SIZE>(obj), __INIT() { };
		StateVector(const StateBase<SIMPLE_STATE_VECTOR_SIZE>& obj) :
			StateBase<SIMPLE_STATE_VECTOR_SIZE>(obj), __INIT() { };
		virtual ~StateVector() {};

		virtual StateVector& operator=(const StateVector& rhs) {
			if (this == &rhs)  return *this;		// If same object skip assignment, and just return *this.
			for (unsigned int i = 0; i < this->_dim; i++) this->data[i] = rhs.data[i];
			return *this;
		};
		virtual StateVector& operator=(const StateDerivativeVector& rhs) {
			for (unsigned int i = 0; i < 3; i++) _u[i] = rhs._u[i];
			return *this;
		};
		virtual const StateVector operator+(const StateVector& obj) const {
			StateVector result;
			for (unsigned int i = 0; i < this->_dim; i++) result.data[i] = this->data[i] + obj.data[i];
			return result;
		};
		virtual const StateVector operator+(const StateDerivativeVector& obj) const {
			StateVector result;
			for (unsigned int i = 0; i < this->_dim; i++) result.data[i] = this->data[i];
			for (unsigned int i = 0; i < 3; i++) result._u[i] += obj._u[i];
			return result;
		};


		double* const _p;
		double* const _R;
		double* const _u;

	protected:

	private:

	};

#undef __INIT


#define __INIT() _p_u0(this->data+0), _ws_u0(this->data+9), _u_u0(this->data+18)

	class IVPJacobiansMini : public StateBase<IVP_JACOBIAN_MINI_STATE_SIZE> {
	public:
		IVPJacobiansMini() : StateBase<IVP_JACOBIAN_MINI_STATE_SIZE>(), __INIT() { };
		IVPJacobiansMini(const IVPJacobiansMini& obj) :						// copy constructor
			StateBase<IVP_JACOBIAN_MINI_STATE_SIZE>(obj), __INIT() { };
		IVPJacobiansMini(const StateBase<IVP_JACOBIAN_MINI_STATE_SIZE>& obj) :
			StateBase<IVP_JACOBIAN_MINI_STATE_SIZE>(obj), __INIT() { };
		virtual ~IVPJacobiansMini() {};

		virtual IVPJacobiansMini& operator=(const IVPJacobiansMini& rhs) {
			if (this == &rhs)  return *this;		// If same object skip assignment, and just return *this.
			for (unsigned int i = 0; i < this->_dim; i++) this->data[i] = rhs.data[i];
			return *this;
		};

		friend std::ostream& operator<<(std::ostream& os, const IVPJacobiansMini& obj) {
			os << "JIVP_p_u0=[ \n";
			__EMT<3, 3> JIVP_p_u0(obj._p_u0);
			os << JIVP_p_u0 << "];\n";
			os << "JIVP_ws_u0=[ \n";
			__EMT<3, 3> JIVP_ws_u0(obj._ws_u0);
			os << JIVP_ws_u0 << "];\n";
			os << "JIVP_u_u0=[ \n";
			__EMT<3, 3> JIVP_u_u0(obj._u_u0);
			os << JIVP_u_u0 << "];\n";

			return os;
		};

		double* const _p_u0;
		double* const _ws_u0;
		double* const _u_u0;

	protected:

	private:

	};

#undef __INIT



	//#define __INIT() _p_p0(this->data + 0), _p_w0(this->data + 9), _p_u0(this->data + 18), _ws_p0(this->data + 27), _ws_w0(this->data + 36), _ws_u0(this->data + 45), _u_p0(this->data + 54), _u_w0(this->data + 63), _u_u0(this->data + 72), _p_zl(this->data + 81), _ws_zl(this->data + 84), _u_zl(this->data + 87), _p_ft(this->data + 90), _ws_ft(this->data + 99), _u_ft(this->data + 108), _p_zc(this->data + 117), _ws_zc(this->data + 117 + 3 * CURRENT_ACT_VECTOR_DIM), _u_zc(this->data + 117 + 6 * CURRENT_ACT_VECTOR_DIM)
#define __INIT() _p_u0(this->data+0), _ws_u0(this->data+9), _u_u0(this->data+18), _p_zl(this->data+27), _ws_zl(this->data+30), _u_zl(this->data+33), _p_ft(this->data+36), _ws_ft(this->data+45), _u_ft(this->data+54), _p_zc(this->data+63), _ws_zc(this->data+63+3*CURRENT_ACT_VECTOR_DIM), _u_zc(this->data+63+6*CURRENT_ACT_VECTOR_DIM)

	class IVPJacobiansFull : public StateBase<IVP_JACOBIAN_FULL_STATE_SIZE> {
	public:
		IVPJacobiansFull() : StateBase<IVP_JACOBIAN_FULL_STATE_SIZE>(), __INIT() { };
		IVPJacobiansFull(const IVPJacobiansFull& obj) :						// copy constructor
			StateBase<IVP_JACOBIAN_FULL_STATE_SIZE>(obj), __INIT() { };
		IVPJacobiansFull(const StateBase<IVP_JACOBIAN_FULL_STATE_SIZE>& obj) :
			StateBase<IVP_JACOBIAN_FULL_STATE_SIZE>(obj), __INIT() { };
		virtual ~IVPJacobiansFull() {};

		virtual IVPJacobiansFull& operator=(const IVPJacobiansFull& rhs) {
			if (this == &rhs)  return *this;		// If same object skip assignment, and just return *this.
			for (unsigned int i = 0; i < this->_dim; i++) this->data[i] = rhs.data[i];
			return *this;
		};

		friend std::ostream& operator<<(std::ostream& os, const IVPJacobiansFull& obj) {
			os << "JIVP_p_u0=[ \n";
			__EMT<3, 3> JIVP_p_u0(obj._p_u0);
			os << JIVP_p_u0 << "];\n";
			os << "JIVP_p_zc=[ \n";
			__EMT<3, 3 * NUM_ACT_SET> JIVP_p_zc(obj._p_zc);
			os << JIVP_p_zc << "];\n";
			os << "JIVP_p_zl=[ \n";
			__EVT<3> JIVP_p_zl(obj._p_zl);
			os << JIVP_p_zl << "];\n";
			os << "JIVP_p_ft=[ \n";
			__EMT<3, 3> JIVP_p_ft(obj._p_ft);
			os << JIVP_p_ft << "];\n";
			os << "\n";
			os << "JIVP_ws_u0=[ \n";
			__EMT<3, 3> JIVP_ws_u0(obj._ws_u0);
			os << JIVP_ws_u0 << "];\n";
			os << "JIVP_ws_zc=[ \n";
			__EMT<3, 3 * NUM_ACT_SET> JIVP_ws_zc(obj._ws_zc);
			os << JIVP_ws_zc << "];\n";
			os << "JIVP_ws_zl=[ \n";
			__EVT<3> JIVP_ws_zl(obj._ws_zl);
			os << JIVP_ws_zl << "];\n";
			os << "JIVP_ws_ft=[ \n";
			__EMT<3, 3> JIVP_ws_ft(obj._ws_ft);
			os << JIVP_ws_ft << "];\n";
			os << "\n";
			os << "JIVP_u_u0=[ \n";
			__EMT<3, 3> JIVP_u_u0(obj._u_u0);
			os << JIVP_u_u0 << "];\n";
			os << "JIVP_u_zc=[ \n";
			__EMT<3, 3 * NUM_ACT_SET> JIVP_u_zc(obj._u_zc);
			os << JIVP_u_zc << "];\n";
			os << "JIVP_u_zl=[ \n";
			__EVT<3> JIVP_u_zl(obj._u_zl);
			os << JIVP_u_zl << "];\n";
			os << "JIVP_u_ft=[ \n";
			__EMT<3, 3> JIVP_u_ft(obj._u_ft);
			os << JIVP_u_ft << "];\n";

			return os;
		};

		double* const _p_u0;
		double* const _p_zc;
		double* const _p_zl;
		double* const _p_ft;
		double* const _ws_u0;
		double* const _ws_zc;
		double* const _ws_zl;
		double* const _ws_ft;
		double* const _u_u0;
		double* const _u_zc;
		double* const _u_zl;
		double* const _u_ft;

	protected:

	private:

	};

#undef __INIT



	template <typename IVPJacobians>
	class AugmentedStateDerivativeVector : public StateDerivativeVector, public IVPJacobians {
	public:
		AugmentedStateDerivativeVector() : StateDerivativeVector(), IVPJacobians() { };
		AugmentedStateDerivativeVector(const AugmentedStateDerivativeVector<IVPJacobians>& obj) :						// copy constructor
			StateDerivativeVector(obj), IVPJacobians(obj) { };
		virtual ~AugmentedStateDerivativeVector() {};

		virtual AugmentedStateDerivativeVector<IVPJacobians>& operator=(const AugmentedStateDerivativeVector<IVPJacobians>& rhs) {
			if (this == &rhs)  return *this;		// If same object skip assignment, and just return *this.
			StateDerivativeVector::operator=(rhs);
			IVPJacobians::operator=(rhs);
			return *this;
		};
		virtual const AugmentedStateDerivativeVector<IVPJacobians> operator+(const AugmentedStateDerivativeVector<IVPJacobians>& obj) const {
			AugmentedStateDerivativeVector<IVPJacobians> result;
			static_cast<StateDerivativeVector&>(result) = StateDerivativeVector::operator+(obj);
			static_cast<IVPJacobians&>(result) = IVPJacobians::operator+(obj);
			return result;
		};
		const AugmentedStateDerivativeVector<IVPJacobians> operator*(const double& scalar) {
			AugmentedStateDerivativeVector<IVPJacobians> result;
			static_cast<StateDerivativeVector&>(result) = StateDerivativeVector::operator*(scalar);
			static_cast<IVPJacobians&>(result) = IVPJacobians::operator*(scalar);
			return result;
		};
		friend const AugmentedStateDerivativeVector<IVPJacobians> operator*(const double& scalar, const AugmentedStateDerivativeVector<IVPJacobians>& obj) {
			AugmentedStateDerivativeVector<IVPJacobians> result;
			static_cast<StateDerivativeVector&>(result) = scalar * static_cast<const StateDerivativeVector&>(obj);
			static_cast<IVPJacobians&>(result) = scalar * static_cast<const IVPJacobians&>(obj);
			return result;
		};

		friend std::ostream& operator<<(std::ostream& os, const AugmentedStateDerivativeVector<IVPJacobians>& obj) {
			os << static_cast<const StateDerivativeVector&>(obj) << "\n" << static_cast<const IVPJacobians&>(obj);
			return os;
		};


	protected:

	private:

	};


	template <typename IVPJacobians>
	class AugmentedStateVector : public StateVector, public IVPJacobians {
	public:
		AugmentedStateVector() : StateVector(), IVPJacobians() { };
		AugmentedStateVector(const AugmentedStateVector<IVPJacobians>& obj) :						// copy constructor
			StateVector(obj), IVPJacobians(obj) { };
		virtual ~AugmentedStateVector() {};


		virtual AugmentedStateVector<IVPJacobians>& operator=(const AugmentedStateVector<IVPJacobians>& rhs) {
			if (this == &rhs)  return *this;		// If same object skip assignment, and just return *this.
			StateVector::operator=(rhs);
			IVPJacobians::operator=(rhs);
			return *this;
		};
		virtual const AugmentedStateVector<IVPJacobians> operator+(const AugmentedStateVector<IVPJacobians>& obj) const {
			AugmentedStateVector<IVPJacobians> result;
			static_cast<StateVector&>(result) = StateVector::operator+(static_cast<const StateVector&>(obj));
			static_cast<IVPJacobians&>(result) = IVPJacobians::operator+(static_cast<const IVPJacobians&>(obj));
			return result;
		};
		virtual const AugmentedStateVector<IVPJacobians> operator*(const double& scalar) {
			AugmentedStateVector<IVPJacobians> result;
			static_cast<StateVector&>(result) = StateVector::operator*(scalar);
			static_cast<IVPJacobians&>(result) = IVPJacobians::operator*(scalar);
			return result;
		};
		friend const AugmentedStateVector<IVPJacobians> operator*(const double& scalar, const AugmentedStateVector<IVPJacobians>& obj) {
			AugmentedStateVector<IVPJacobians> result;
			static_cast<StateVector&>(result) = scalar * static_cast<const StateVector&>(obj);
			static_cast<IVPJacobians&>(result) = scalar * static_cast<const IVPJacobians&>(obj);
			return result;
		};
		virtual AugmentedStateVector<IVPJacobians>& operator=(const AugmentedStateDerivativeVector<IVPJacobians>& rhs) {
			//if (this == &rhs)  return *this;		// If same object skip assignment, and just return *this.  // they cannot be the same, so no need for this
			StateVector::operator=(static_cast<const StateDerivativeVector&>(rhs));
			IVPJacobians::operator=(rhs);
			return *this;
		};
		virtual const AugmentedStateVector<IVPJacobians> operator+(const AugmentedStateDerivativeVector<IVPJacobians>& obj) const {
			AugmentedStateVector<IVPJacobians> result;
			static_cast<StateVector&>(result) = StateVector::operator+(static_cast<const StateDerivativeVector&>(obj));
			static_cast<IVPJacobians&>(result) = IVPJacobians::operator+(obj);
			return result;
		};

		double absmax() {
			return MAX(static_cast<StateVector*>(this)->absmax(), static_cast<IVPJacobians*>(this)->absmax());
		};


		friend std::ostream& operator<<(std::ostream& os, const AugmentedStateVector<IVPJacobians>& obj) {
			os << static_cast<const StateVector&>(obj) << "\n" << static_cast<const IVPJacobians&>(obj);
			return os;
		};


	protected:

	private:

	};

}
