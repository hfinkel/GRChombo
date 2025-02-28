/* GRChombo
 * Copyright 2012 The GRChombo collaboration.
 * Please refer to LICENSE in GRChombo's root directory.
 */

#if !defined(WEYL4_HPP_)
#error "This file should only be included through Weyl4.hpp"
#endif

#ifndef WEYL4_IMPL_HPP_
#define WEYL4_IMPL_HPP_

template <class data_t> void Weyl4::compute(Cell<data_t> current_cell) const
{

    // copy data from chombo gridpoint into local variables
    const auto vars = current_cell.template load_vars<Vars>();
    const auto d1 = m_deriv.template diff1<Vars>(current_cell);
    const auto d2 = m_deriv.template diff2<Diff2Vars>(current_cell);

    // Get the coordinates
    const Coordinates<data_t> coords(current_cell, m_dx, m_center);

    // Compute the E and B fields
    EBFields_t<data_t> ebfields = compute_EB_fields(vars, d1, d2, coords);

    // work out the Newman Penrose scalar
    NPScalar_t<data_t> out = compute_Weyl4(ebfields, vars, d1, d2, coords);

    // Write the rhs into the output FArrayBox
    current_cell.store_vars(out.Real, c_Weyl4_Re);
    current_cell.store_vars(out.Im, c_Weyl4_Im);
}

// Calculation of E and B fields, using tetrads from gr-qc/0104063
// Formalism from Alcubierre book
template <class data_t>
EBFields_t<data_t>
Weyl4::compute_EB_fields(const Vars<data_t> &vars,
                         const Vars<Tensor<1, data_t>> &d1,
                         const Diff2Vars<Tensor<2, data_t>> &d2,
                         const Coordinates<data_t> &coords) const
{
    EBFields_t<data_t> out;

    // raised normal vector, NB index 3 is time
    data_t n_U[4];
    n_U[3] = 1. / vars.lapse;
    FOR1(i) { n_U[i] = -vars.shift[i] / vars.lapse; }

    // 4D levi civita symbol and 3D levi civita tensor in LLL and LUU form
    const std::array<std::array<std::array<std::array<int, 4>, 4>, 4>, 4>
        epsilon4 = TensorAlgebra::epsilon4D();
    Tensor<3, data_t> epsilon3_LLL;
    Tensor<3, data_t> epsilon3_LUU;

    // Projection of antisymmentric Tensor onto hypersurface - see 8.3.17,
    // Alcubierre
    FOR3(i, j, k)
    {
        epsilon3_LLL[i][j][k] = 0.0;
        epsilon3_LUU[i][j][k] = 0.0;
    }
    // projection of 4-antisymetric tensor to 3-tensor on hypersurface
    // note last index contracted as per footnote 86 pg 290 Alcubierre
    FOR3(i, j, k)
    {
        for (int l = 0; l < 4; ++l)
        {
            epsilon3_LLL[i][j][k] += n_U[l] * epsilon4[i][j][k][l] *
                                     vars.lapse / (vars.chi * sqrt(vars.chi));
        }
    }
    // rasing indices
    const auto h_UU = TensorAlgebra::compute_inverse_sym(vars.h);
    FOR3(i, j, k)
    {
        FOR2(m, n)
        {
            epsilon3_LUU[i][j][k] += epsilon3_LLL[i][m][n] * h_UU[m][j] *
                                     vars.chi * h_UU[n][k] * vars.chi;
        }
    }

    // Extrinsic curvature
    Tensor<2, data_t> K_tensor;
    Tensor<3, data_t> d1_K_tensor;
    Tensor<3, data_t> covariant_deriv_K_tensor;

    // Compute inverse, Christoffel symbols and Ricci Tensor
    using namespace TensorAlgebra;
    const auto chris = compute_christoffel(d1.h, h_UU);
    const auto ricci = CCZ4Geometry::compute_ricci(vars, d1, d2, h_UU, chris);

    // Compute full spatial Christoffel symbols
    const Tensor<3, data_t> chris_phys =
        compute_phys_chris(d1.chi, vars.chi, vars.h, h_UU, chris.ULL);

    // Extrinsic curvature and corresponding covariant and partial derivatives
    FOR2(i, j)
    {
        K_tensor[i][j] = vars.A[i][j] / vars.chi +
                         1. / 3. * (vars.h[i][j] * vars.K) / vars.chi;

        FOR1(k)
        {
            d1_K_tensor[i][j][k] = d1.A[i][j][k] / vars.chi -
                                   d1.chi[k] / vars.chi * K_tensor[i][j] +
                                   1. / 3. * d1.h[i][j][k] * vars.K / vars.chi +
                                   1. / 3. * vars.h[i][j] * d1.K[k] / vars.chi;
        }
    }
    // covariant derivative of K
    FOR3(i, j, k) { covariant_deriv_K_tensor[i][j][k] = d1_K_tensor[i][j][k]; }

    FOR4(i, j, k, l)
    {
        covariant_deriv_K_tensor[i][j][k] +=
            -chris_phys[l][k][i] * K_tensor[l][j] -
            chris_phys[l][k][j] * K_tensor[i][l];
    }

    // Calculate electric and magnetic fields
    FOR2(i, j)
    {
        out.E[i][j] = 0;
        out.B[i][j] = 0;
    }

    FOR4(i, j, k, l)
    {
        out.B[i][j] +=
            epsilon3_LUU[i][k][l] * (covariant_deriv_K_tensor[l][j][k]);
    }

    FOR2(i, j) { out.E[i][j] += ricci.LL[i][j] + vars.K * K_tensor[i][j]; }

    FOR4(i, j, k, l)
    {
        out.E[i][j] += -K_tensor[i][k] * K_tensor[l][j] * h_UU[k][l] * vars.chi;
    }

    return out;
}

// Calculation of the Weyl4 scalar
template <class data_t>
NPScalar_t<data_t> Weyl4::compute_Weyl4(const EBFields_t<data_t> &ebfields,
                                        const Vars<data_t> &vars,
                                        const Vars<Tensor<1, data_t>> &d1,
                                        const Diff2Vars<Tensor<2, data_t>> &d2,
                                        const Coordinates<data_t> &coords) const
{
    NPScalar_t<data_t> out;

    // Calculate the tetrads
    const Tetrad_t<data_t> tetrad = compute_null_tetrad(vars, coords);

    // Projection of Electric and magnetic field components using tetrads
    out.Real = 0.0;
    out.Im = 0.0;
    FOR2(i, j)
    {
        out.Real += 0.5 * (ebfields.E[i][j] * (tetrad.w[i] * tetrad.w[j] -
                                               tetrad.v[i] * tetrad.v[j]) -
                           2.0 * ebfields.B[i][j] * tetrad.w[i] * tetrad.v[j]);
        out.Im += 0.5 * (ebfields.B[i][j] * (-tetrad.w[i] * tetrad.w[j] +
                                             tetrad.v[i] * tetrad.v[j]) -
                         2.0 * ebfields.E[i][j] * tetrad.w[i] * tetrad.v[j]);
    }

    return out;
}

// Calculation of the null tetrad
// Defintions from gr-qc/0104063
// "The Lazarus project: A pragmatic approach to binary black hole evolutions",
// Baker et al.
template <class data_t>
Tetrad_t<data_t>
Weyl4::compute_null_tetrad(const Vars<data_t> &vars,
                           const Coordinates<data_t> &coords) const
{
    Tetrad_t<data_t> out;

    // compute coords
    const data_t x = coords.x;
    const double y = coords.y;
    const double z = coords.z;

    // the inverse metric and alternating levi civita symbol
    const auto h_UU = TensorAlgebra::compute_inverse_sym(vars.h);
    const Tensor<3, double> epsilon = TensorAlgebra::epsilon();

    // calculate the tetrad
    out.u[0] = x;
    out.u[1] = y;
    out.u[2] = z;

    out.v[0] = -y;
    out.v[1] = x;
    out.v[2] = 0.0;

    out.w[0] = 0.0;
    out.w[1] = 0.0;
    out.w[2] = 0.0;

    // floor on chi
    const data_t chi = simd_max(vars.chi, 1e-4);

    FOR4(i, j, k, m)
    {
        out.w[i] += 1. / sqrt(chi) * h_UU[i][j] * epsilon[j][k][m] * out.v[k] *
                    out.u[m];
    }

    // Gram Schmitt orthonormalisation
    // Choice of orthonormalisaion to avoid frame-dragging
    data_t omega_11 = 0.0;
    FOR2(i, j) { omega_11 += out.v[i] * out.v[j] * vars.h[i][j] / chi; }
    FOR1(i) { out.v[i] = out.v[i] / sqrt(omega_11); }

    data_t omega_12 = 0.0;
    FOR2(i, j) { omega_12 += out.v[i] * out.u[j] * vars.h[i][j] / chi; }
    FOR1(i) { out.u[i] += -omega_12 * out.v[i]; }

    data_t omega_22 = 0.0;
    FOR2(i, j) { omega_22 += out.u[i] * out.u[j] * vars.h[i][j] / chi; }
    FOR1(i) { out.u[i] = out.u[i] / sqrt(omega_22); }

    data_t omega_13 = 0.0;
    data_t omega_23 = 0.0;
    FOR2(i, j)
    {
        omega_13 += out.v[i] * out.w[j] * vars.h[i][j] / chi;
        omega_23 += out.u[i] * out.w[j] * vars.h[i][j] / chi;
    }
    FOR1(i) { out.w[i] += -(omega_13 * out.v[i] + omega_23 * out.u[i]); }

    data_t omega_33 = 0.0;
    FOR2(i, j) { omega_33 += out.w[i] * out.w[j] * vars.h[i][j] / chi; }
    FOR1(i) { out.w[i] = out.w[i] / sqrt(omega_33); }

    return out;
}

#endif /* WEYL4_HPP_ */
