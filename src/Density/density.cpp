// Copyright (c) 2013-2019 Anton Kozhevnikov, Thomas Schulthess
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without modification, are permitted provided that
// the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the
//    following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions
//    and the following disclaimer in the documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
// WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
// PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
// OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

/** \file density.cpp
 *
 *  \brief Contains implementation of sirius::Density class.
 */

#include "density.hpp"
#include "../Symmetry/symmetrize.hpp"

namespace sirius {

Density::Density(Simulation_context& ctx__)
    : Field4D(ctx__, ctx__.lmmax_rho())
    , unit_cell_(ctx_.unit_cell())
{
    PROFILE("sirius::Density");

    if (!ctx_.initialized()) {
        TERMINATE("Simulation_context is not initialized");
    }

    using spf = Smooth_periodic_function<double>;

    /*  allocate charge density and magnetization on a coarse grid */
    for (int i = 0; i < ctx_.num_mag_dims() + 1; i++) {
        rho_mag_coarse_[i] = std::unique_ptr<spf>(new spf(ctx_.fft_coarse(), ctx_.gvec_coarse_partition()));
    }

    /* core density of the pseudopotential method */
    if (!ctx_.full_potential()) {
        rho_pseudo_core_ = std::unique_ptr<spf>(new spf(ctx_.fft(), ctx_.gvec_partition()));
    }

    if (ctx_.full_potential()) {
        using gc_z   = Gaunt_coefficients<double_complex>;
        gaunt_coefs_ = std::unique_ptr<gc_z>(new gc_z(ctx_.lmax_apw(), ctx_.lmax_rho(), ctx_.lmax_apw(), SHT::gaunt_hybrid));
    }

    l_by_lm_ = utils::l_by_lm(ctx_.lmax_rho());

    density_matrix_ = mdarray<double_complex, 4>(unit_cell_.max_mt_basis_size(), unit_cell_.max_mt_basis_size(),
                                                 ctx_.num_mag_comp(), unit_cell_.num_atoms());
    density_matrix_.zero();

    /* split local G-vectors to low-frequency and high-frequency */
    for (int igloc = 0; igloc < ctx_.gvec().count(); igloc++) {
        int  ig = ctx_.gvec().offset() + igloc;
        auto gv = ctx_.gvec().gvec_cart<index_domain_t::local>(igloc);
        if (ig) {
            gvec_mixer_weights_.push_back(fourpi * unit_cell_.omega() / std::pow(gv.length(), 2));
        } else {
            gvec_mixer_weights_.push_back(0);
        }
    }

    update();
}

void
Density::update()
{
    PROFILE("sirius::Density::update");

    if (!ctx_.full_potential()) {
        rho_pseudo_core_->zero();
        bool is_empty{true};
        for (int iat = 0; iat < unit_cell_.num_atom_types(); iat++) {
            is_empty &= unit_cell_.atom_type(iat).ps_core_charge_density().empty();
        }
        if (!is_empty) {
            generate_pseudo_core_charge_density();
        }
    }
}

/// Find the total leakage of the core states out of the muffin-tins
double
Density::core_leakage() const
{
    double sum = 0.0;
    for (int ic = 0; ic < unit_cell_.num_atom_symmetry_classes(); ic++) {
        sum += core_leakage(ic) * unit_cell_.atom_symmetry_class(ic).num_atoms();
    }
    return sum;
}

void
Density::initial_density()
{
    PROFILE("sirius::Density::initial_density");

    zero();

    if (ctx_.full_potential()) {
        initial_density_full_pot();
    } else {
        initial_density_pseudo();

        init_paw();

        init_density_matrix_for_paw();

        generate_paw_loc_density();
    }
}

void
Density::initial_density_pseudo()
{
    auto v = ctx_.make_periodic_function<index_domain_t::local>([&](int iat, double g)
                                                                {
                                                                    return ctx_.ps_rho_ri().value<int>(iat, g);
                                                                });

    if (ctx_.control().print_checksum_) {
        auto z1 = mdarray<double_complex, 1>(&v[0], ctx_.gvec().count()).checksum();
        ctx_.comm().allreduce(&z1, 1);
        if (ctx_.comm().rank() == 0) {
            utils::print_checksum("rho_pw_init", z1);
        }
    }
    std::copy(v.begin(), v.end(), &rho().f_pw_local(0));

    double charge = rho().f_0().real() * unit_cell_.omega();

    if (std::abs(charge - unit_cell_.num_valence_electrons()) > 1e-6) {
        std::stringstream s;
        s << "wrong initial charge density" << std::endl
          << "  integral of the density : " << std::setprecision(12) << charge << std::endl
          << "  target number of electrons : " << std::setprecision(12) << unit_cell_.num_valence_electrons();
        if (ctx_.comm().rank() == 0) {
            WARNING(s);
        }
    }
    rho().fft_transform(1);

    /* remove possible negative noise */
    for (int ir = 0; ir < ctx_.fft().local_size(); ir++) {
        rho().f_rg(ir) = std::max(rho().f_rg(ir), 0.0);
    }
    /* renormalize charge */
    normalize();

    if (ctx_.control().print_checksum_) {
        auto cs = rho().checksum_rg();
        if (ctx_.comm().rank() == 0) {
            utils::print_checksum("rho_rg", cs);
        }
    }

    /* initialize the magnetization */
    if (ctx_.num_mag_dims()) {
        double R = ctx_.control().rmt_max_;

        auto w = [R](double x)
        {
            /* the constants are picked in such a way that the volume integral of the
               weight function is equal to the volume of the atomic sphere;
               in this case the starting magnetiation in the atomic spehre
               integrates to the starting magnetization vector */

            /* volume of the sphere */
            const double norm = fourpi * std::pow(R, 3) / 3.0;
            return (35.0 / 8) * std::pow(1 - std::pow(x / R, 2), 2) / norm;
            //return 10 * std::pow(1 - x / R, 2) / norm;
            //const double b = 1.1016992073677703;
            //return b * 1.0 /  (std::exp(10 * (a - R)) + 1) / norm;
            //const double norm = pi * std::pow(R, 3) / 3.0;
            //return 1.0 / (std::exp(10 * (x - R)) + 1) / norm;
       };

        #pragma omp parallel for
        for (int ia = 0; ia < unit_cell_.num_atoms(); ia++) {
            auto& atom_to_grid_map = ctx_.atoms_to_grid_idx_map(ia);
            vector3d<double> v = unit_cell_.atom(ia).vector_field();

            for (auto coord: atom_to_grid_map) {
                int ir   = coord.first;
                double a = coord.second;
                magnetization(0).f_rg(ir) += v[2] * w(a);
                if (ctx_.num_mag_dims() == 3) {
                    magnetization(1).f_rg(ir) += v[0] * w(a);
                    magnetization(2).f_rg(ir) += v[1] * w(a);
                }
            }
        }
    }

    if (ctx_.control().print_checksum_) {
        for (int i = 0; i < ctx_.num_mag_dims() + 1; i++) {
            auto cs = component(i).checksum_rg();
            if (ctx_.comm().rank() == 0) {
                std::stringstream s;
                s << "component[" << i << "]";
                utils::print_checksum(s.str(), cs);
            }
        }
    }

    rho().fft_transform(-1);
    for (int j = 0; j < ctx_.num_mag_dims(); j++) {
        magnetization(j).fft_transform(-1);
    }

    //if (ctx_.control().print_checksum_ && ctx_.comm().rank() == 0) {
    //    double_complex cs = mdarray<double_complex, 1>(&rho_->f_pw(0), ctx_.gvec().num_gvec()).checksum();
    //    DUMP("checksum(rho_pw): %20.14f %20.14f", std::real(cs), std::imag(cs));
    //}
}

void
Density::initial_density_full_pot()
{
    /* initialize smooth density of free atoms */
    for (int iat = 0; iat < unit_cell_.num_atom_types(); iat++) {
        unit_cell_.atom_type(iat).init_free_atom_density(true);
    }

    /* compute radial integrals */
    Radial_integrals_rho_free_atom ri(ctx_.unit_cell(), ctx_.pw_cutoff(), 40);

    /* compute contribution from free atoms to the interstitial density */
    auto v = ctx_.make_periodic_function<index_domain_t::local>([&ri](int iat, double g)
                                                                {
                                                                    return ri.value(iat, g);
                                                                });

    /* initialize density of free atoms (not smoothed) */
    for (int iat = 0; iat < unit_cell_.num_atom_types(); iat++) {
        unit_cell_.atom_type(iat).init_free_atom_density(false);
    }

    if (ctx_.control().print_checksum_) {
        auto z = mdarray<double_complex, 1>(&v[0], ctx_.gvec().count()).checksum();
        ctx_.comm().allreduce(&z, 1);
        if (ctx_.comm().rank() == 0) {
            utils::print_checksum("rho_pw", z);
        }
    }

    /* set plane-wave coefficients of the charge density */
    std::copy(v.begin(), v.end(), &rho().f_pw_local(0));
    /* convert charge density to real space mesh */
    rho().fft_transform(1);

    if (ctx_.control().print_checksum_) {
        auto cs = rho().checksum_rg();
        if (ctx_.comm().rank() == 0) {
            utils::print_checksum("rho_rg", cs);
        }
    }

    /* remove possible negative noise */
    for (int ir = 0; ir < ctx_.fft().local_size(); ir++) {
        rho().f_rg(ir) = std::max(0.0, rho().f_rg(ir));
    }

    /* set Y00 component of charge density */
    for (int ia = 0; ia < ctx_.unit_cell().num_atoms(); ia++) {
        int nmtp = ctx_.unit_cell().atom(ia).num_mt_points();

        for (int ir = 0; ir < nmtp; ir++) {
            double x = ctx_.unit_cell().atom(ia).radial_grid(ir);
            rho().f_mt<index_domain_t::global>(0, ir, ia) = unit_cell_.atom(ia).type().free_atom_density(x) / y00;
        }
    }

    int lmax = ctx_.lmax_rho();
    int lmmax = utils::lmmax(lmax);

    auto l_by_lm = utils::l_by_lm(lmax);

    std::vector<double_complex> zil(lmax + 1);
    for (int l = 0; l <= lmax; l++) {
        zil[l] = std::pow(double_complex(0, 1), l);
    }

    /* compute boundary value at MT sphere from the plane-wave exapansion */
    auto gvec_ylm = ctx_.generate_gvec_ylm(lmax);

    auto sbessel_mt = ctx_.generate_sbessel_mt(lmax);

    auto flm = ctx_.sum_fg_fl_yg(lmax, v.data(), sbessel_mt, gvec_ylm);

    /* this is the difference between the value of periodic charge density at MT boundary and
       a value of the atom's free density at the boundary */
    for (int ia = 0; ia < ctx_.unit_cell().num_atoms(); ia++) {
        double R = ctx_.unit_cell().atom(ia).mt_radius();
        double c = unit_cell_.atom(ia).type().free_atom_density(R) / y00;
        flm(0, ia) -= c;
    }

    /* match density at MT */
    for (int iat = 0; iat < ctx_.unit_cell().num_atom_types(); iat++) {
        mdarray<double, 2> rRl(ctx_.unit_cell().max_num_mt_points(), lmax + 1);
        double R = ctx_.unit_cell().atom_type(iat).mt_radius();
        int nmtp = ctx_.unit_cell().atom_type(iat).num_mt_points();

        #pragma omp parallel for default(shared)
        for (int l = 0; l <= lmax; l++) {
            for (int ir = 0; ir < nmtp; ir++) {
                rRl(ir, l) = std::pow(ctx_.unit_cell().atom_type(iat).radial_grid(ir) / R, 2);
            }
        }
        #pragma omp parallel for default(shared)
        for (int i = 0; i < unit_cell_.atom_type(iat).num_atoms(); i++) {
            int ia = unit_cell_.atom_type(iat).atom_id(i);
            std::vector<double> glm(lmmax);
            SHT::convert(lmax, &flm(0, ia), &glm[0]);
            for (int lm = 0; lm < lmmax; lm++) {
                int l = l_by_lm[lm];
                for (int ir = 0; ir < nmtp; ir++) {
                    rho().f_mt<index_domain_t::global>(lm, ir, ia) += glm[lm] * rRl(ir, l);
                }
            }
        }
    }

    /* normalize charge density */
    normalize();

    check_num_electrons();

    //FILE* fout = fopen("rho.dat", "w");
    //for (int i = 0; i <= 10000; i++) {
    //    vector3d<double> v = (i / 10000.0) * vector3d<double>({10.26, 10.26, 10.26});
    //    double val = rho().value(v);
    //    fprintf(fout, "%18.12f %18.12f\n", v.length(), val);
    //}
    //fclose(fout);

    //FILE* fout2 = fopen("rho_rg.dat", "w");
    //for (int i = 0; i <= 10000; i++) {
    //    vector3d<double> v = (i / 10000.0) * vector3d<double>({10.26, 10.26, 10.26});
    //    double val = rho().value_rg(v);
    //    fprintf(fout2, "%18.12f %18.12f\n", v.length(), val);
    //}
    //fclose(fout2);

    /* initialize the magnetization */
    if (ctx_.num_mag_dims()) {
        for (int ialoc = 0; ialoc < unit_cell_.spl_num_atoms().local_size(); ialoc++) {
            int ia = unit_cell_.spl_num_atoms(ialoc);
            vector3d<double> v = unit_cell_.atom(ia).vector_field();
            double len = v.length();

            int nmtp = unit_cell_.atom(ia).num_mt_points();
            Spline<double> rho_s(unit_cell_.atom(ia).type().radial_grid());
            double R = unit_cell_.atom(ia).mt_radius();
            for (int ir = 0; ir < nmtp; ir++) {
                double x = unit_cell_.atom(ia).type().radial_grid(ir);
                rho_s(ir) = this->rho().f_mt<index_domain_t::local>(0, ir, ialoc) * y00 * (1 - 3 * std::pow(x / R, 2) + 2 * std::pow(x / R, 3));
            }

            /* maximum magnetization which can be achieved if we smooth density towards MT boundary */
            double q = fourpi * rho_s.interpolate().integrate(2);

            /* if very strong initial magnetization is given */
            if (q < len) {
                /* renormalize starting magnetization */
                for (int x: {0, 1, 2}) {
                    v[x] *= (q / len);
                }
                len = q;
            }

            if (len > 1e-8) {
                for (int ir = 0; ir < nmtp; ir++) {
                    magnetization(0).f_mt<index_domain_t::local>(0, ir, ialoc) = rho_s(ir) * v[2] / q / y00;
                }
                if (ctx_.num_mag_dims() == 3) {
                    for (int ir = 0; ir < nmtp; ir++) {
                        magnetization(1).f_mt<index_domain_t::local>(0, ir, ialoc) = rho_s(ir) * v[0] / q / y00;
                        magnetization(2).f_mt<index_domain_t::local>(0, ir, ialoc) = rho_s(ir) * v[1] / q / y00;
                    }
                }
            }
        }
    }
}

void Density::init_paw()
{
    paw_density_data_.clear();

    if (!unit_cell_.num_paw_atoms()) {
        return;
    }

    for (int i = 0; i < unit_cell_.spl_num_paw_atoms().local_size(); i++) {
        int   ia_paw    = unit_cell_.spl_num_paw_atoms(i);
        int   ia        = unit_cell_.paw_atom_index(ia_paw);
        auto& atom      = unit_cell_.atom(ia);
        auto& atom_type = atom.type();

        int l_max      = 2 * atom_type.indexr().lmax_lo();
        int lm_max_rho = utils::lmmax(l_max);

        paw_density_data_t pdd;

        pdd.atom_ = &atom;

        pdd.ia = ia;

        // allocate density arrays
        for (int i = 0; i < ctx_.num_mag_dims() + 1; i++) {
            pdd.ae_density_.push_back(Spheric_function<function_domain_t::spectral, double>(lm_max_rho, pdd.atom_->radial_grid()));
            pdd.ps_density_.push_back(Spheric_function<function_domain_t::spectral, double>(lm_max_rho, pdd.atom_->radial_grid()));
        }

        paw_density_data_.push_back(std::move(pdd));
    }
}

void Density::init_density_matrix_for_paw()
{
    density_matrix_.zero();

    for (int ipaw = 0; ipaw < unit_cell_.num_paw_atoms(); ipaw++) {
        int ia = unit_cell_.paw_atom_index(ipaw);

        auto& atom      = unit_cell_.atom(ia);
        auto& atom_type = atom.type();

        int nbf = atom_type.mt_basis_size();

        auto& occupations = atom_type.paw_wf_occ();

        /* magnetization vector */
        auto magn = atom.vector_field();

        for (int xi = 0; xi < nbf; xi++) {
            auto& basis_func_index_dsc = atom_type.indexb()[xi];

            int rad_func_index = basis_func_index_dsc.idxrf;

            double occ = occupations[rad_func_index];

            int l = basis_func_index_dsc.l;

            switch (ctx_.num_mag_dims()) {
                case 0: {
                    density_matrix_(xi, xi, 0, ia) = occ / double(2 * l + 1);
                    break;
                }

                case 3:
                case 1: {
                    double nm                      = (std::abs(magn[2]) < 1.0) ? magn[2] : std::copysign(1, magn[2]);
                    density_matrix_(xi, xi, 0, ia) = 0.5 * (1.0 + nm) * occ / double(2 * l + 1);
                    density_matrix_(xi, xi, 1, ia) = 0.5 * (1.0 - nm) * occ / double(2 * l + 1);
                    break;
                }
            }
        }
    }
}

void Density::generate_paw_atom_density(paw_density_data_t& pdd)
{
    int ia = pdd.ia;

    auto& atom_type = pdd.atom_->type();

    auto l_by_lm = utils::l_by_lm(2 * atom_type.indexr().lmax_lo());

    /* get gaunt coefficients */
    Gaunt_coefficients<double> GC(atom_type.indexr().lmax_lo(), 2 * atom_type.indexr().lmax_lo(),
                                  atom_type.indexr().lmax_lo(), SHT::gaunt_rlm);

    for (int i = 0; i < ctx_.num_mag_dims() + 1; i++) {
        pdd.ae_density_[i].zero();
        pdd.ps_density_[i].zero();
    }

    /* get radial grid to divide density over r^2 */
    auto& grid = atom_type.radial_grid();

    auto& paw_ae_wfs = atom_type.ae_paw_wfs_array();
    auto& paw_ps_wfs = atom_type.ps_paw_wfs_array();

    /* iterate over local basis functions (or over lm1 and lm2) */
    for (int xi2 = 0; xi2 < atom_type.indexb().size(); xi2++) {
        int lm2  = atom_type.indexb(xi2).lm;
        int irb2 = atom_type.indexb(xi2).idxrf;

        for (int xi1 = 0; xi1 <= xi2; xi1++) {
            int lm1  = atom_type.indexb(xi1).lm;
            int irb1 = atom_type.indexb(xi1).idxrf;

            /* get num of non-zero GC */
            int num_non_zero_gk = GC.num_gaunt(lm1, lm2);

            double diag_coef = (xi1 == xi2) ? 1.0 : 2.0;

            /* store density matrix in aux form */
            double dm[4] = {0, 0, 0, 0};
            switch (ctx_.num_mag_dims()) {
                case 3: {
                    dm[2] = 2 * std::real(density_matrix_(xi1, xi2, 2, ia));
                    dm[3] = -2 * std::imag(density_matrix_(xi1, xi2, 2, ia));
                }
                case 1: {
                    dm[0] = std::real(density_matrix_(xi1, xi2, 0, ia) + density_matrix_(xi1, xi2, 1, ia));
                    dm[1] = std::real(density_matrix_(xi1, xi2, 0, ia) - density_matrix_(xi1, xi2, 1, ia));
                    break;
                }
                case 0: {
                    dm[0] = std::real(density_matrix_(xi1, xi2, 0, ia));
                }
            }

            for (int imagn = 0; imagn < ctx_.num_mag_dims() + 1; imagn++) {
                auto& ae_dens = pdd.ae_density_[imagn];
                auto& ps_dens = pdd.ps_density_[imagn];

                /* add nonzero coefficients */
                for (int inz = 0; inz < num_non_zero_gk; inz++) {
                    auto& lm3coef = GC.gaunt(lm1, lm2, inz);

                    /* iterate over radial points */
                    for (int irad = 0; irad < grid.num_points(); irad++) {

                        /* we need to divide density over r^2 since wave functions are stored multiplied by r */
                        double inv_r2 = diag_coef / (grid[irad] * grid[irad]);

                        /* calculate unified density/magnetization
                         * dm_ij * GauntCoef * ( phi_i phi_j  +  Q_ij) */
                        ae_dens(lm3coef.lm3, irad) += dm[imagn] * inv_r2 * lm3coef.coef * paw_ae_wfs(irad, irb1) * paw_ae_wfs(irad, irb2);
                        ps_dens(lm3coef.lm3, irad) += dm[imagn] * inv_r2 * lm3coef.coef *
                                                      (paw_ps_wfs(irad, irb1) * paw_ps_wfs(irad, irb2) + atom_type.q_radial_function(irb1, irb2, l_by_lm[lm3coef.lm3])(irad));
                    }
                }
            }
        }
    }
}

void Density::generate_paw_loc_density()
{
    if (!unit_cell_.num_paw_atoms()) {
        return;
    }

    #pragma omp parallel for
    for (int i = 0; i < unit_cell_.spl_num_paw_atoms().local_size(); i++) {
        generate_paw_atom_density(paw_density_data_[i]);
    }
}

void Density::add_k_point_contribution_rg(K_point* kp__)
{
    PROFILE("sirius::Density::add_k_point_contribution_rg");

    double omega = unit_cell_.omega();

    auto& fft = ctx_.fft_coarse();

    /* get preallocated memory */
    mdarray<double, 2> density_rg(ctx_.mem_pool(memory_t::host), fft.local_size(), ctx_.num_mag_dims() + 1, "density_rg");
    density_rg.zero();

    if (fft.pu() == device_t::GPU) {
        density_rg.allocate(ctx_.mem_pool(memory_t::device));
        density_rg.zero(memory_t::device);
    }

    fft.prepare(kp__->gkvec_partition());

    /* non-magnetic or collinear case */
    if (ctx_.num_mag_dims() != 3) {
        /* loop over pure spinor components */
        for (int ispn = 0; ispn < ctx_.num_spins(); ispn++) {
            /* trivial case */
            if (!kp__->spinor_wave_functions().pw_coeffs(ispn).spl_num_col().global_index_size()) {
                continue;
            }

            for (int i = 0; i < kp__->spinor_wave_functions().pw_coeffs(ispn).spl_num_col().local_size(); i++) {
                int j = kp__->spinor_wave_functions().pw_coeffs(ispn).spl_num_col()[i];
                double w = kp__->band_occupancy(j, ispn) * kp__->weight() / omega;

                /* transform to real space; in case of GPU wave-function stays in GPU memory */
                fft.transform<1>(kp__->spinor_wave_functions().pw_coeffs(ispn).extra().at(memory_t::host, 0, i));
                /* add to density */
                switch (fft.pu()) {
                    case device_t::CPU: {
                        #pragma omp parallel for schedule(static)
                        for (int ir = 0; ir < fft.local_size(); ir++) {
                            auto z = fft.buffer(ir);
                            density_rg(ir, ispn) += w * (std::pow(z.real(), 2) + std::pow(z.imag(), 2));
                        }
                        break;
                    }
                    case device_t::GPU: {
#ifdef __GPU
                        update_density_rg_1_gpu(fft.local_size(), fft.buffer().at(memory_t::device), w,
                                                density_rg.at(memory_t::device, 0, ispn));
#endif
                        break;
                    }
                }
            }
        }
    } else { /* non-collinear case */
        assert(kp__->spinor_wave_functions().pw_coeffs(0).spl_num_col().local_size() ==
               kp__->spinor_wave_functions().pw_coeffs(1).spl_num_col().local_size());

        /* allocate on CPU or GPU */
        mdarray<double_complex, 1> psi_r(ctx_.mem_pool(memory_t::host), fft.local_size());
        if (fft.pu() == device_t::GPU) {
            psi_r.allocate(ctx_.mem_pool(memory_t::device));
        }
        for (int i = 0; i < kp__->spinor_wave_functions().pw_coeffs(0).spl_num_col().local_size(); i++) {
            int j    = kp__->spinor_wave_functions().pw_coeffs(0).spl_num_col()[i];
            double w = kp__->band_occupancy(j, 0) * kp__->weight() / omega;

            /* transform up- component of spinor function to real space; in case of GPU wave-function stays in GPU memory */
            fft.transform<1>(kp__->spinor_wave_functions().pw_coeffs(0).extra().at(memory_t::host, 0, i));
            /* save in auxiliary buffer */
            switch (fft.pu()) {
                case device_t::CPU: {
                    fft.output(&psi_r[0]);
                    break;
                }
                case device_t::GPU: {
#ifdef __GPU
                    acc::copyout(psi_r.at(memory_t::device), fft.buffer().at(memory_t::device), fft.local_size());
#endif
                    break;
                }
            }

            /* transform dn- component of spinor wave function */
            fft.transform<1>(kp__->spinor_wave_functions().pw_coeffs(1).extra().at(memory_t::host, 0, i));

            switch (fft.pu()) {
                case device_t::CPU: {
                    #pragma omp parallel for schedule(static)
                    for (int ir = 0; ir < fft.local_size(); ir++) {
                        auto r0 = (std::pow(psi_r[ir].real(), 2) + std::pow(psi_r[ir].imag(), 2)) * w;
                        auto r1 = (std::pow(fft.buffer(ir).real(), 2) + std::pow(fft.buffer(ir).imag(), 2)) * w;

                        auto z2 = psi_r[ir] * std::conj(fft.buffer(ir)) * w;

                        density_rg(ir, 0) += r0;
                        density_rg(ir, 1) += r1;
                        density_rg(ir, 2) += 2.0 * std::real(z2);
                        density_rg(ir, 3) -= 2.0 * std::imag(z2);
                    }
                    break;
                }
                case device_t::GPU: {
#ifdef __GPU
                    /* add up-up contribution */
                    update_density_rg_1_gpu(fft.local_size(), psi_r.at(memory_t::device), w,
                                            density_rg.at(memory_t::device, 0, 0));
                    /* add dn-dn contribution */
                    update_density_rg_1_gpu(fft.local_size(), fft.buffer().at(memory_t::device), w,
                                            density_rg.at(memory_t::device, 0, 1));
                    /* add off-diagonal contribution */
                    update_density_rg_2_gpu(fft.local_size(), psi_r.at(memory_t::device), fft.buffer().at(memory_t::device), w,
                                            density_rg.at(memory_t::device, 0, 2), density_rg.at(memory_t::device, 0, 3));
#endif
                    break;
                }
            }
        }
    }

    if (fft.pu() == device_t::GPU) {
        density_rg.copy_to(memory_t::host);
    }

    /* switch from real density matrix to density and magnetization */
    switch (ctx_.num_mag_dims()) {
        case 3: {
            #pragma omp parallel for schedule(static)
            for (int ir = 0; ir < fft.local_size(); ir++) {
                rho_mag_coarse_[2]->f_rg(ir) += density_rg(ir, 2); // Mx
                rho_mag_coarse_[3]->f_rg(ir) += density_rg(ir, 3); // My
            }
        }
        case 1: {
            #pragma omp parallel for schedule(static)
            for (int ir = 0; ir < fft.local_size(); ir++) {
                rho_mag_coarse_[0]->f_rg(ir) += (density_rg(ir, 0) + density_rg(ir, 1)); // rho
                rho_mag_coarse_[1]->f_rg(ir) += (density_rg(ir, 0) - density_rg(ir, 1)); // Mz
            }
            break;
        }
        case 0: {
            #pragma omp parallel for schedule(static)
            for (int ir = 0; ir < fft.local_size(); ir++) {
                rho_mag_coarse_[0]->f_rg(ir) += density_rg(ir, 0); // rho
            }
        }
    }

    fft.dismiss();
}

template <typename T>
void Density::add_k_point_contribution_dm(K_point* kp__, mdarray<double_complex, 4>& density_matrix__)
{
    PROFILE("sirius::Density::add_k_point_contribution_dm");

    if (ctx_.full_potential()) {
        /* non-magnetic or spin-collinear case */
        if (ctx_.num_mag_dims() != 3) {
            for (int ispn = 0; ispn < ctx_.num_spins(); ispn++) {
                int nbnd = kp__->num_occupied_bands(ispn);

                mdarray<double_complex, 2> wf1(unit_cell_.max_mt_basis_size(), nbnd);
                mdarray<double_complex, 2> wf2(unit_cell_.max_mt_basis_size(), nbnd);

                for (int ialoc = 0; ialoc < kp__->spinor_wave_functions().spl_num_atoms().local_size(); ialoc++) {
                    int ia            = kp__->spinor_wave_functions().spl_num_atoms()[ialoc];
                    int mt_basis_size = unit_cell_.atom(ia).type().mt_basis_size();
                    int offset_wf     = kp__->spinor_wave_functions().offset_mt_coeffs(ialoc);
                    if (mt_basis_size != 0) {
                        for (int i = 0; i < nbnd; i++) {
                            for (int xi = 0; xi < mt_basis_size; xi++) {
                                auto c = kp__->spinor_wave_functions().mt_coeffs(ispn).prime(offset_wf + xi, i);
                                wf1(xi, i) = std::conj(c);
                                wf2(xi, i) = c * kp__->band_occupancy(i, ispn) * kp__->weight();
                            }
                        }
                        /* add |psi_j> n_j <psi_j| to density matrix */
                        linalg<device_t::CPU>::gemm(0, 1, mt_basis_size, mt_basis_size, nbnd, linalg_const<double_complex>::one(),
                                                    &wf1(0, 0), wf1.ld(), &wf2(0, 0), wf2.ld(), linalg_const<double_complex>::one(),
                                                    density_matrix__.at(memory_t::host, 0, 0, ispn, ia), density_matrix__.ld());
                    }
                }
            }
        } else {
            int nbnd = kp__->num_occupied_bands();

            mdarray<double_complex, 3> wf1(unit_cell_.max_mt_basis_size(), nbnd, ctx_.num_spins());
            mdarray<double_complex, 3> wf2(unit_cell_.max_mt_basis_size(), nbnd, ctx_.num_spins());

            for (int ialoc = 0; ialoc < kp__->spinor_wave_functions().spl_num_atoms().local_size(); ialoc++) {
                int ia            = kp__->spinor_wave_functions().spl_num_atoms()[ialoc];
                int mt_basis_size = unit_cell_.atom(ia).type().mt_basis_size();
                int offset_wf     = kp__->spinor_wave_functions().offset_mt_coeffs(ialoc);

                if (mt_basis_size != 0) {
                    for (int ispn = 0; ispn < ctx_.num_spins(); ispn++) {
                        for (int i = 0; i < nbnd; i++) {

                            for (int xi = 0; xi < mt_basis_size; xi++) {
                                auto c = kp__->spinor_wave_functions().mt_coeffs(ispn).prime(offset_wf + xi, i);
                                wf1(xi, i, ispn) = std::conj(c);
                                wf2(xi, i, ispn) = c * kp__->band_occupancy(i, 0) * kp__->weight();
                            }
                        }
                    }
                    /* compute diagonal terms */
                    for (int ispn = 0; ispn < 2; ispn++) {
                        linalg<device_t::CPU>::gemm(0, 1, mt_basis_size, mt_basis_size, nbnd, linalg_const<double_complex>::one(),
                                                    &wf1(0, 0, ispn), wf1.ld(), &wf2(0, 0, ispn), wf2.ld(),
                                                    linalg_const<double_complex>::one(), density_matrix__.at(memory_t::host, 0, 0, ispn, ia),
                                                    density_matrix__.ld());
                    }
                    /* offdiagonal term */
                    linalg<device_t::CPU>::gemm(0, 1, mt_basis_size, mt_basis_size, nbnd, linalg_const<double_complex>::one(),
                                                &wf1(0, 0, 1), wf1.ld(), &wf2(0, 0, 0), wf2.ld(), linalg_const<double_complex>::one(),
                                                density_matrix__.at(memory_t::host, 0, 0, 2, ia), density_matrix__.ld());
                }
            }
        }
    } else { /* pseudopotential */
        if (!ctx_.unit_cell().mt_lo_basis_size()) {
            return;
        }

        kp__->beta_projectors().prepare();

        if (ctx_.num_mag_dims() != 3) {
            for (int chunk = 0; chunk < kp__->beta_projectors().num_chunks(); chunk++) {
                kp__->beta_projectors().generate(chunk);

                for (int ispn = 0; ispn < ctx_.num_spins(); ispn++) {
                    /* total number of occupied bands for this spin */
                    int nbnd = kp__->num_occupied_bands(ispn);
                    /* compute <beta|psi> */
                    auto beta_psi = kp__->beta_projectors().inner<T>(chunk, kp__->spinor_wave_functions(), ispn, 0, nbnd);

                    /* number of beta projectors */
                    int nbeta = kp__->beta_projectors().chunk(chunk).num_beta_;

                    /* use communicator of the k-point to split band index */
                    splindex<splindex_t::block> spl_nbnd(nbnd, kp__->comm().size(), kp__->comm().rank());

                    int nbnd_loc = spl_nbnd.local_size();
                    if (nbnd_loc) { // TODO: this part can also be moved to GPU
                    #pragma omp parallel
                    {
                        /* auxiliary arrays */
                        mdarray<double_complex, 2> bp1(nbeta, nbnd_loc);
                        mdarray<double_complex, 2> bp2(nbeta, nbnd_loc);
                        #pragma omp for
                        for (int ia = 0; ia < kp__->beta_projectors().chunk(chunk).num_atoms_; ia++) {
                            int nbf  = kp__->beta_projectors().chunk(chunk).desc_(static_cast<int>(beta_desc_idx::nbf), ia);
                            int offs = kp__->beta_projectors().chunk(chunk).desc_(static_cast<int>(beta_desc_idx::offset), ia);
                            int ja   = kp__->beta_projectors().chunk(chunk).desc_(static_cast<int>(beta_desc_idx::ia), ia);

                            for (int i = 0; i < nbnd_loc; i++) {
                                int j = spl_nbnd[i];

                                for (int xi = 0; xi < nbf; xi++) {
                                    bp1(xi, i) = beta_psi(offs + xi, j);
                                    bp2(xi, i) = std::conj(bp1(xi, i)) * kp__->weight() *
                                                 kp__->band_occupancy(j, ispn);
                                }
                            }

                            linalg<device_t::CPU>::gemm(0, 1, nbf, nbf, nbnd_loc, linalg_const<double_complex>::one(),
                                                        &bp1(0, 0), bp1.ld(), &bp2(0, 0), bp2.ld(),
                                                        linalg_const<double_complex>::one(),
                                                        &density_matrix__(0, 0, ispn, ja),
                                                        density_matrix__.ld());
                        }
                    }
                    }
                }
            }
        } else {
            for (int chunk = 0; chunk < kp__->beta_projectors().num_chunks(); chunk++) {
                kp__->beta_projectors().generate(chunk);

                /* number of beta projectors */
                int nbeta = kp__->beta_projectors().chunk(chunk).num_beta_;

                /* total number of occupied bands */
                int nbnd = kp__->num_occupied_bands();

                splindex<splindex_t::block> spl_nbnd(nbnd, kp__->comm().size(), kp__->comm().rank());
                int nbnd_loc = spl_nbnd.local_size();

                /* auxiliary arrays */
                mdarray<double_complex, 3> bp1(nbeta, nbnd_loc, ctx_.num_spins());
                mdarray<double_complex, 3> bp2(nbeta, nbnd_loc, ctx_.num_spins());

                for (int ispn = 0; ispn < ctx_.num_spins(); ispn++) {
                    /* compute <beta|psi> */
                    auto beta_psi = kp__->beta_projectors().inner<T>(chunk, kp__->spinor_wave_functions(), ispn, 0, nbnd);
                    #pragma omp parallel for schedule(static)
                    for (int i = 0; i < nbnd_loc; i++) {
                        int j = spl_nbnd[i];

                        for (int m = 0; m < nbeta; m++) {
                            bp1(m, i, ispn) = beta_psi(m, j);
                            bp2(m, i, ispn) = std::conj(beta_psi(m, j)) * kp__->weight() * kp__->band_occupancy(j, 0);
                        }
                    }
                }
                for (int ia = 0; ia < kp__->beta_projectors().chunk(chunk).num_atoms_; ia++) {
                    int nbf  = kp__->beta_projectors().chunk(chunk).desc_(static_cast<int>(beta_desc_idx::nbf), ia);
                    int offs = kp__->beta_projectors().chunk(chunk).desc_(static_cast<int>(beta_desc_idx::offset), ia);
                    int ja   = kp__->beta_projectors().chunk(chunk).desc_(static_cast<int>(beta_desc_idx::ia), ia);
                    if (ctx_.unit_cell().atom(ja).type().spin_orbit_coupling()) {
                        mdarray<double_complex, 3> bp3(nbf, nbnd_loc, 2);
                        bp3.zero();
                        /* We already have the <beta|psi> but we need to rotate
                         *  them when the spin orbit interaction is included in the
                         *  pseudo potential.
                         *
                         *  We rotate \f[\langle\beta|\psi\rangle\f] accordingly by multiplying it with
                         *  the \f[f^{\sigma\sigma^{'}}_{\xi,\xi^'}\f]
                         */

                        for (int xi1 = 0; xi1 < nbf; xi1++) {
                            for (int i = 0; i < nbnd_loc; i++) {
                                for (int xi1p = 0; xi1p < nbf; xi1p++) {
                                    if (ctx_.unit_cell().atom(ja).type().compare_index_beta_functions(xi1, xi1p)) {
                                        bp3(xi1, i, 0) +=
                                            bp1(offs + xi1p, i, 0) *
                                                ctx_.unit_cell().atom(ja).type().f_coefficients(xi1, xi1p, 0, 0) +
                                            bp1(offs + xi1p, i, 1) *
                                                ctx_.unit_cell().atom(ja).type().f_coefficients(xi1, xi1p, 0, 1);
                                        bp3(xi1, i, 1) +=
                                            bp1(offs + xi1p, i, 0) *
                                                ctx_.unit_cell().atom(ja).type().f_coefficients(xi1, xi1p, 1, 0) +
                                            bp1(offs + xi1p, i, 1) *
                                                ctx_.unit_cell().atom(ja).type().f_coefficients(xi1, xi1p, 1, 1);
                                    }
                                }
                            }
                        }

                        for (int xi1 = 0; xi1 < nbf; xi1++) {
                            for (int i = 0; i < nbnd_loc; i++) {
                                bp1(offs + xi1, i, 0) = bp3(xi1, i, 0);
                                bp1(offs + xi1, i, 1) = bp3(xi1, i, 1);
                            }
                        }

                        bp3.zero();

                        for (int xi1 = 0; xi1 < nbf; xi1++) {
                            for (int i = 0; i < nbnd_loc; i++) {
                                for (int xi1p = 0; xi1p < nbf; xi1p++) {
                                    if (ctx_.unit_cell().atom(ja).type().compare_index_beta_functions(xi1, xi1p)) {
                                        bp3(xi1, i, 0) +=
                                            bp2(offs + xi1p, i, 0) *
                                                ctx_.unit_cell().atom(ja).type().f_coefficients(xi1p, xi1, 0, 0) +
                                            bp2(offs + xi1p, i, 1) *
                                                ctx_.unit_cell().atom(ja).type().f_coefficients(xi1p, xi1, 1, 0);
                                        bp3(xi1, i, 1) +=
                                            bp2(offs + xi1p, i, 0) *
                                                ctx_.unit_cell().atom(ja).type().f_coefficients(xi1p, xi1, 0, 1) +
                                            bp2(offs + xi1p, i, 1) *
                                                ctx_.unit_cell().atom(ja).type().f_coefficients(xi1p, xi1, 1, 1);
                                    }
                                }
                            }
                        }

                        for (int xi1 = 0; xi1 < nbf; xi1++) {
                            for (int i = 0; i < nbnd_loc; i++) {
                                bp2(offs + xi1, i, 0) = bp3(xi1, i, 0);
                                bp2(offs + xi1, i, 1) = bp3(xi1, i, 1);
                            }
                        }
                    }
                }

                if (nbnd_loc) {
                    #pragma omp parallel for
                    for (int ia = 0; ia < kp__->beta_projectors().chunk(chunk).num_atoms_; ia++) {
                        int nbf  = kp__->beta_projectors().chunk(chunk).desc_(static_cast<int>(beta_desc_idx::nbf), ia);
                        int offs = kp__->beta_projectors().chunk(chunk).desc_(static_cast<int>(beta_desc_idx::offset), ia);
                        int ja   = kp__->beta_projectors().chunk(chunk).desc_(static_cast<int>(beta_desc_idx::ia), ia);
                        /* compute diagonal spin blocks */
                        for (int ispn = 0; ispn < 2; ispn++) {
                            linalg<device_t::CPU>::gemm(0, 1, nbf, nbf, nbnd_loc, linalg_const<double_complex>::one(),
                                                        &bp1(offs, 0, ispn), bp1.ld(), &bp2(offs, 0, ispn), bp2.ld(),
                                                        linalg_const<double_complex>::one(), &density_matrix__(0, 0, ispn, ja),
                                                        density_matrix__.ld());
                        }
                        /* off-diagonal spin block */
                        linalg<device_t::CPU>::gemm(0, 1, nbf, nbf, nbnd_loc, linalg_const<double_complex>::one(),
                                                    &bp1(offs, 0, 0), bp1.ld(), &bp2(offs, 0, 1), bp2.ld(),
                                                    linalg_const<double_complex>::one(), &density_matrix__(0, 0, 2, ja),
                                                    density_matrix__.ld());
                    }
                }
            }
        }
        kp__->beta_projectors().dismiss();
    }
}

void Density::normalize()
{
    double nel = std::get<0>(rho().integrate());
    double scale = unit_cell_.num_electrons() / nel;

    /* renormalize interstitial part */
    for (int ir = 0; ir < ctx_.fft().local_size(); ir++) {
         rho().f_rg(ir) *= scale;
    }
    if (ctx_.full_potential()) {
        for (int ia = 0; ia < unit_cell_.num_atoms(); ia++) {
            for (int ir = 0; ir < unit_cell_.atom(ia).num_mt_points(); ir++) {
                for (int lm = 0; lm < ctx_.lmmax_rho(); lm++) {
                    rho().f_mt<index_domain_t::global>(lm, ir, ia) *= scale;
                }
            }
        }
    }
}

/// Check total density for the correct number of electrons.
bool Density::check_num_electrons() const
{
    double nel{0};
    if (ctx_.full_potential()) {
        nel = std::get<0>(rho().integrate());
    } else {
        nel = rho().f_0().real() * unit_cell_.omega();
    }

    /* check the number of electrons */
    if (std::abs(nel - unit_cell_.num_electrons()) > 1e-5 && ctx_.comm().rank() == 0) {
        std::stringstream s;
        s << "wrong number of electrons" << std::endl
          << "  obtained value : " << nel << std::endl
          << "  target value : " << unit_cell_.num_electrons() << std::endl
          << "  difference : " << std::abs(nel - unit_cell_.num_electrons()) << std::endl;
        if (ctx_.full_potential()) {
            s << "  total core leakage : " << core_leakage();
            for (int ic = 0; ic < unit_cell_.num_atom_symmetry_classes(); ic++) {
                s << std::endl
                  << "    atom class : " << ic << ", core leakage : " << core_leakage(ic);
            }
        }
        WARNING(s);
        return false;
    } else {
        return true;
    }
}


void Density::generate(K_point_set const& ks__, bool add_core__, bool transform_to_rg__)
{
    PROFILE("sirius::Density::generate");

    generate_valence(ks__);

    if (ctx_.full_potential()) {
        if (add_core__) {
            /* find the core states */
            generate_core_charge_density();
            /* add core contribution */
            for (int ialoc = 0; ialoc < (int)unit_cell_.spl_num_atoms().local_size(); ialoc++) {
                int ia = unit_cell_.spl_num_atoms(ialoc);
                for (int ir = 0; ir < unit_cell_.atom(ia).num_mt_points(); ir++) {
                    rho().f_mt<index_domain_t::local>(0, ir, ialoc) +=
                        unit_cell_.atom(ia).symmetry_class().ae_core_charge_density(ir) / y00;
                }
            }
        }
        /* synchronize muffin-tin part */
        for (int iv = 0; iv < ctx_.num_mag_dims() + 1; iv++) {
            this->component(iv).sync_mt();
        }
    }
    if (transform_to_rg__) {
        for (int iv = 0; iv < ctx_.num_mag_dims() + 1; iv++) {
            this->component(iv).fft_transform(1);
        }
    }
}

void Density::augment()
{
    PROFILE("sirius::Density::augment");

    /*check if we need to augment charge density and magnetization */
    bool need_to_augment{false};
    for (int iat = 0; iat < unit_cell_.num_atom_types(); iat++) {
        need_to_augment |= unit_cell_.atom_type(iat).augment();
    }
    if (!need_to_augment) {
        return;
    }

    //if (ctx_.control().print_checksum_) {
    //    for (auto e: rho_vec_) {
    //        auto cs = e->checksum_pw();
    //        DUMP("checksum(rho_vec_pw): %20.14f %20.14f", cs.real(), cs.imag());
    //    }
    //}

    //mdarray<double_complex, 2> rho_aug(ctx_.gvec().count(), ctx_.num_mag_dims() + 1);

    auto rho_aug = generate_rho_aug();

    for (int iv = 0; iv < ctx_.num_mag_dims() + 1; iv++) {
        #pragma omp parallel for schedule(static)
        for (int igloc = 0; igloc < ctx_.gvec().count(); igloc++) {
            this->component(iv).f_pw_local(igloc) += rho_aug(igloc, iv);
        }
    }
}

void Density::generate_valence(K_point_set const& ks__)
{
    PROFILE("sirius::Density::generate_valence");

    /* check weights */
    double wt{0};
    double occ_val{0};
    for (int ik = 0; ik < ks__.num_kpoints(); ik++) {
        wt += ks__[ik]->weight();
        for (int ispn = 0; ispn < ctx_.num_spin_dims(); ispn++) {
            for (int j = 0; j < ctx_.num_bands(); j++) {
                occ_val += ks__[ik]->weight() * ks__[ik]->band_occupancy(j, ispn);
            }
        }
    }

    if (std::abs(wt - 1.0) > 1e-12) {
        std::stringstream s;
        s << "K_point weights don't sum to one" << std::endl
          << "  obtained sum: " << wt;
        TERMINATE(s);
    }

    if (std::abs(occ_val - unit_cell_.num_valence_electrons() + ctx_.parameters_input().extra_charge_) > 1e-8 &&
        ctx_.comm().rank() == 0) {
        std::stringstream s;
        s << "wrong band occupancies" << std::endl
          << "  computed : " << occ_val << std::endl
          << "  required : " << unit_cell_.num_valence_electrons() - ctx_.parameters_input().extra_charge_ << std::endl
          << "  difference : " << std::abs(occ_val - unit_cell_.num_valence_electrons() + ctx_.parameters_input().extra_charge_);
        WARNING(s);
    }

    density_matrix_.zero();

    /* zero density and magnetization */
    zero();
    for (int i = 0; i < ctx_.num_mag_dims() + 1; i++) {
        rho_mag_coarse_[i]->zero();
    }

    /* start the main loop over k-points */
    for (int ikloc = 0; ikloc < ks__.spl_num_kpoints().local_size(); ikloc++) {
        int ik = ks__.spl_num_kpoints(ikloc);
        auto kp = ks__[ik];

        for (int ispn = 0; ispn < ctx_.num_spins(); ispn++) {
            int nbnd = kp->num_occupied_bands(ispn);
            if (is_device_memory(ctx_.preferred_memory_t())) {
                /* allocate GPU memory */
                kp->spinor_wave_functions().pw_coeffs(ispn).prime().allocate(ctx_.mem_pool(memory_t::device));
                kp->spinor_wave_functions().pw_coeffs(ispn).copy_to(memory_t::device, 0, nbnd); // TODO: copy this asynchronously
            }
            /* swap wave functions for the FFT transformation */
            kp->spinor_wave_functions().pw_coeffs(ispn).remap_forward(nbnd, 0, &ctx_.mem_pool(memory_t::host));
        }

        if (ctx_.electronic_structure_method() == electronic_structure_method_t::full_potential_lapwlo) {
            add_k_point_contribution_dm<double_complex>(kp, density_matrix_);
        }

        if (ctx_.electronic_structure_method() == electronic_structure_method_t::pseudopotential) {
            if (ctx_.gamma_point() && (ctx_.so_correction() == false)) {
                add_k_point_contribution_dm<double>(kp, density_matrix_);
            } else {
                add_k_point_contribution_dm<double_complex>(kp, density_matrix_);
            }
        }

        /* add contribution from regular space grid */
        add_k_point_contribution_rg(kp);

        if (is_device_memory(ctx_.preferred_memory_t())) {
            for (int ispn = 0; ispn < ctx_.num_spins(); ispn++) {
                /* deallocate GPU memory */
                kp->spinor_wave_functions().pw_coeffs(ispn).deallocate(memory_t::device);
            }
        }
    }

    if (density_matrix_.size()) {
        ctx_.comm().allreduce(density_matrix_.at(memory_t::host), static_cast<int>(density_matrix_.size()));
    }

    ctx_.fft_coarse().prepare(ctx_.gvec_coarse_partition());
    auto& comm = ctx_.gvec_coarse_partition().comm_ortho_fft();
    for (int j = 0; j < ctx_.num_mag_dims() + 1; j++) {
        /* reduce arrays; assume that each rank did its own fraction of the density */
        /* comm_ortho_fft is idential to a product of column communicator inside k-point with k-point communicator */
        comm.allreduce(&rho_mag_coarse_[j]->f_rg(0), ctx_.fft_coarse().local_size());
        /* print checksum if needed */
        if (ctx_.control().print_checksum_) {
            auto cs = mdarray<double, 1>(&rho_mag_coarse_[j]->f_rg(0), ctx_.fft_coarse().local_size()).checksum();
            ctx_.fft_coarse().comm().allreduce(&cs, 1);
            if (ctx_.comm().rank() == 0) {
                utils::print_checksum("rho_mag_coarse_rg", cs);
            }
        }
        /* transform to PW domain */
        rho_mag_coarse_[j]->fft_transform(-1);
        /* map to fine G-vector grid */
        for (int igloc = 0; igloc < ctx_.gvec_coarse().count(); igloc++) {
            component(j).f_pw_local(ctx_.gvec().gvec_base_mapping(igloc)) = rho_mag_coarse_[j]->f_pw_local(igloc);
        }
    }
    ctx_.fft_coarse().dismiss();

    if (!ctx_.full_potential()) {
        augment();

        /* remove extra chanrge */
        if (ctx_.gvec().comm().rank() == 0) {
            rho().f_pw_local(0) += ctx_.parameters_input().extra_charge_ / ctx_.unit_cell().omega();

        }

        if (ctx_.control().print_hash_ && ctx_.comm().rank() == 0) {
            auto h = mdarray<double_complex, 1>(&rho().f_pw_local(0), ctx_.gvec().count()).hash();
            utils::print_hash("rho", h);
        }

        double nel = rho().f_0().real() * unit_cell_.omega();
        /* check the number of electrons */
        if (std::abs(nel - unit_cell_.num_electrons()) > 1e-8 && ctx_.comm().rank() == 0) {
            std::stringstream s;
            s << "wrong unsymmetrized density" << std::endl
              << "  obtained value : " << std::scientific << nel << std::endl
              << "  target value : " << std::scientific << unit_cell_.num_electrons() << std::endl
              << "  difference : " << std::scientific << std::abs(nel - unit_cell_.num_electrons()) << std::endl;
            WARNING(s);
        }
    }

    /* for muffin-tin part */
    if (ctx_.full_potential()) {
        generate_valence_mt();
    }
}

mdarray<double_complex, 2>
Density::generate_rho_aug()
{
    PROFILE("sirius::Density::generate_rho_aug");

    auto spl_ngv_loc = ctx_.split_gvec_local();

    mdarray<double_complex, 2> rho_aug(ctx_.gvec().count(), ctx_.num_mag_dims() + 1);
    switch (ctx_.processing_unit()) {
        case device_t::CPU: {
            rho_aug.zero(memory_t::host);
            break;
        }
        case device_t::GPU: {
            rho_aug.allocate(memory_t::device).zero(memory_t::device);
            break;
        }
    }

    if (ctx_.unit_cell().atom_type(0).augment() && ctx_.unit_cell().atom_type(0).num_atoms() > 0) {
        ctx_.augmentation_op(0).prepare(stream_id(0));
    }

    for (int iat = 0; iat < unit_cell_.num_atom_types(); iat++) {
        auto& atom_type = unit_cell_.atom_type(iat);

        if (ctx_.processing_unit() == device_t::GPU) {
            acc::sync_stream(stream_id(0));
            if (iat + 1 != unit_cell_.num_atom_types() && ctx_.unit_cell().atom_type(iat + 1).augment() &&
                ctx_.unit_cell().atom_type(iat + 1).num_atoms() > 0) {
                ctx_.augmentation_op(iat + 1).prepare(stream_id(0));
            }
        }

        if (!atom_type.augment() || atom_type.num_atoms() == 0) {
            continue;
        }

        int nbf = atom_type.mt_basis_size();

        /* convert to real matrix */
        auto dm = density_matrix_aux(iat);

        if (ctx_.control().print_checksum_) {
             auto cs = dm.checksum();
             if (ctx_.comm().rank() == 0) {
                utils::print_checksum("density_matrix_aux", cs);
             }
        }
        /* treat auxiliary array as double with x2 size */
        mdarray<double, 2> dm_pw(ctx_.mem_pool(memory_t::host), nbf * (nbf + 1) / 2, spl_ngv_loc.local_size() * 2);
        mdarray<double, 2> phase_factors(ctx_.mem_pool(memory_t::host), atom_type.num_atoms(), spl_ngv_loc.local_size() * 2);

        switch (ctx_.processing_unit()) {
            case device_t::CPU: {
                break;
            }
            case device_t::GPU: {
                phase_factors.allocate(ctx_.mem_pool(memory_t::device));
                dm_pw.allocate(ctx_.mem_pool(memory_t::device));
                dm.allocate(ctx_.mem_pool(memory_t::device)).copy_to(memory_t::device);
                break;
            }
        }

        for (int ib = 0; ib < spl_ngv_loc.num_ranks(); ib++) {
            int g_begin = spl_ngv_loc.global_index(0, ib);
            int g_end = g_begin + spl_ngv_loc.local_size(ib);

            switch (ctx_.processing_unit()) {
                case device_t::CPU: {
                    #pragma omp parallel for schedule(static)
                    for (int igloc = g_begin; igloc < g_end; igloc++) {
                        int ig = ctx_.gvec().offset() + igloc;
                        for (int i = 0; i < atom_type.num_atoms(); i++) {
                            int ia = atom_type.atom_id(i);
                            double_complex z = std::conj(ctx_.gvec_phase_factor(ig, ia));
                            phase_factors(i, 2 * (igloc - g_begin))     = z.real();
                            phase_factors(i, 2 * (igloc - g_begin) + 1) = z.imag();
                        }
                    }
                    for (int iv = 0; iv < ctx_.num_mag_dims() + 1; iv++) {
                        utils::timer t3("sirius::Density::generate_rho_aug|gemm");
                        linalg2(linalg_t::blas).gemm('N', 'N', nbf * (nbf + 1) / 2, 2 * spl_ngv_loc.local_size(ib),
                                                     atom_type.num_atoms(),
                                                     &linalg_const<double>::one(),
                                                     dm.at(memory_t::host, 0, 0, iv), dm.ld(),
                                                     phase_factors.at(memory_t::host), phase_factors.ld(),
                                                     &linalg_const<double>::zero(),
                                                     dm_pw.at(memory_t::host, 0, 0), dm_pw.ld());
                        t3.stop();
                        utils::timer t4("sirius::Density::generate_rho_aug|sum");
                        #pragma omp parallel for
                        for (int igloc = g_begin; igloc < g_end; igloc++) {
                            double_complex zsum(0, 0);
                            /* get contribution from non-diagonal terms */
                            for (int i = 0; i < nbf * (nbf + 1) / 2; i++) {
                                double_complex z1 = double_complex(ctx_.augmentation_op(iat).q_pw(i, 2 * igloc),
                                                                   ctx_.augmentation_op(iat).q_pw(i, 2 * igloc + 1));
                                double_complex z2(dm_pw(i, 2 * (igloc - g_begin)), dm_pw(i, 2 * (igloc - g_begin) + 1));

                                zsum += z1 * z2 * ctx_.augmentation_op(iat).sym_weight(i);
                            }
                            rho_aug(igloc, iv) += zsum;
                        }
                        t4.stop();
                    }
                    break;
                }
                case device_t::GPU: {
#if defined(__GPU)
                    for (int iv = 0; iv < ctx_.num_mag_dims() + 1; iv++) {
                        generate_dm_pw_gpu(atom_type.num_atoms(),
                                           spl_ngv_loc.local_size(ib),
                                           nbf,
                                           ctx_.unit_cell().atom_coord(iat).at(memory_t::device),
                                           ctx_.gvec_coord().at(memory_t::device, g_begin, 0),
                                           ctx_.gvec_coord().at(memory_t::device, g_begin, 1),
                                           ctx_.gvec_coord().at(memory_t::device, g_begin, 2),
                                           phase_factors.at(memory_t::device),
                                           dm.at(memory_t::device, 0, 0, iv),
                                           dm_pw.at(memory_t::device),
                                           1);
                        sum_q_pw_dm_pw_gpu(spl_ngv_loc.local_size(ib),
                                           nbf,
                                           ctx_.augmentation_op(iat).q_pw().at(memory_t::device, 0, 2 * g_begin),
                                           dm_pw.at(memory_t::device),
                                           ctx_.augmentation_op(iat).sym_weight().at(memory_t::device),
                                           rho_aug.at(memory_t::device, g_begin, iv),
                                           1);
                    }
#endif
                    break;
                }
            }
        }

        if (ctx_.processing_unit() == device_t::GPU) {
            acc::sync_stream(stream_id(1));
            ctx_.augmentation_op(iat).dismiss();
        }
    }

    if (ctx_.processing_unit() == device_t::GPU) {
        rho_aug.copy_to(memory_t::host);
    }

    if (ctx_.control().print_checksum_) {
         auto cs = rho_aug.checksum();
         ctx_.comm().allreduce(&cs, 1);
         if (ctx_.comm().rank() == 0) {
            utils::print_checksum("rho_aug", cs);
         }
    }

    if (ctx_.control().print_hash_) {
         auto h = rho_aug.hash();
         if (ctx_.comm().rank() == 0) {
            utils::print_hash("rho_aug", h);
         }
    }

    return rho_aug;
}

template <int num_mag_dims>
void Density::reduce_density_matrix(Atom_type const& atom_type__, int ia__, mdarray<double_complex, 4> const& zdens__,
                                    Gaunt_coefficients<double_complex> const& gaunt_coeffs__,
                                    mdarray<double, 3>& mt_density_matrix__)
{
    mt_density_matrix__.zero();

    #pragma omp parallel for default(shared)
    for (int idxrf2 = 0; idxrf2 < atom_type__.mt_radial_basis_size(); idxrf2++) {
        int l2 = atom_type__.indexr(idxrf2).l;
        for (int idxrf1 = 0; idxrf1 <= idxrf2; idxrf1++) {
            int offs = idxrf2 * (idxrf2 + 1) / 2 + idxrf1;
            int l1   = atom_type__.indexr(idxrf1).l;

            int xi2 = atom_type__.indexb().index_by_idxrf(idxrf2);
            for (int lm2 = utils::lm(l2, -l2); lm2 <= utils::lm(l2, l2); lm2++, xi2++) {
                int xi1 = atom_type__.indexb().index_by_idxrf(idxrf1);
                for (int lm1 = utils::lm(l1, -l1); lm1 <= utils::lm(l1, l1); lm1++, xi1++) {
                    for (int k = 0; k < gaunt_coeffs__.num_gaunt(lm1, lm2); k++) {
                        int  lm3 = gaunt_coeffs__.gaunt(lm1, lm2, k).lm3;
                        auto gc  = gaunt_coeffs__.gaunt(lm1, lm2, k).coef;
                        switch (num_mag_dims) {
                            case 3: {
                                mt_density_matrix__(lm3, offs, 2) += 2.0 * std::real(zdens__(xi1, xi2, 2, ia__) * gc);
                                mt_density_matrix__(lm3, offs, 3) -= 2.0 * std::imag(zdens__(xi1, xi2, 2, ia__) * gc);
                            }
                            case 1: {
                                mt_density_matrix__(lm3, offs, 1) += std::real(zdens__(xi1, xi2, 1, ia__) * gc);
                            }
                            case 0: {
                                mt_density_matrix__(lm3, offs, 0) += std::real(zdens__(xi1, xi2, 0, ia__) * gc);
                            }
                        }
                    }
                }
            }
        }
    }
}

void Density::generate_valence_mt()
{
    PROFILE("sirius::Density::generate_valence_mt");

    /* compute occupation matrix */
    if (ctx_.hubbard_correction())
    {
        STOP();

        // TODO: fix the way how occupation matrix is calculated

        //Timer t3("sirius::Density::generate:om");
        //
        //mdarray<double_complex, 4> occupation_matrix(16, 16, 2, 2);
        //
        //for (int ialoc = 0; ialoc < unit_cell_.spl_num_atoms().local_size(); ialoc++)
        //{
        //    int ia = unit_cell_.spl_num_atoms(ialoc);
        //    Atom_type* type = unit_cell_.atom(ia)->type();
        //
        //    occupation_matrix.zero();
        //    for (int l = 0; l <= 3; l++)
        //    {
        //        int num_rf = type->indexr().num_rf(l);

        //        for (int j = 0; j < num_zdmat; j++)
        //        {
        //            for (int order2 = 0; order2 < num_rf; order2++)
        //            {
        //            for (int lm2 = Utils::lm_by_l_m(l, -l); lm2 <= Utils::lm_by_l_m(l, l); lm2++)
        //            {
        //                for (int order1 = 0; order1 < num_rf; order1++)
        //                {
        //                for (int lm1 = Utils::lm_by_l_m(l, -l); lm1 <= Utils::lm_by_l_m(l, l); lm1++)
        //                {
        //                    occupation_matrix(lm1, lm2, dmat_spins_[j].first, dmat_spins_[j].second) +=
        //                        mt_complex_density_matrix_loc(type->indexb_by_lm_order(lm1, order1),
        //                                                      type->indexb_by_lm_order(lm2, order2), j, ialoc) *
        //                        unit_cell_.atom(ia)->symmetry_class()->o_radial_integral(l, order1, order2);
        //                }
        //                }
        //            }
        //            }
        //        }
        //    }
        //
        //    // restore the du block
        //    for (int lm1 = 0; lm1 < 16; lm1++)
        //    {
        //        for (int lm2 = 0; lm2 < 16; lm2++)
        //            occupation_matrix(lm2, lm1, 1, 0) = conj(occupation_matrix(lm1, lm2, 0, 1));
        //    }

        //    unit_cell_.atom(ia)->set_occupation_matrix(&occupation_matrix(0, 0, 0, 0));
        //}

        //for (int ia = 0; ia < unit_cell_.num_atoms(); ia++)
        //{
        //    int rank = unit_cell_.spl_num_atoms().local_rank(ia);
        //    unit_cell_.atom(ia)->sync_occupation_matrix(ctx_.comm(), rank);
        //}
    }

    int max_num_rf_pairs = unit_cell_.max_mt_radial_basis_size() *
                           (unit_cell_.max_mt_radial_basis_size() + 1) / 2;

    // real density matrix
    mdarray<double, 3> mt_density_matrix(ctx_.lmmax_rho(), max_num_rf_pairs, ctx_.num_mag_dims() + 1);

    mdarray<double, 2> rf_pairs(unit_cell_.max_num_mt_points(), max_num_rf_pairs);
    mdarray<double, 3> dlm(ctx_.lmmax_rho(), unit_cell_.max_num_mt_points(),
                           ctx_.num_mag_dims() + 1);

    for (int ialoc = 0; ialoc < unit_cell_.spl_num_atoms().local_size(); ialoc++) {
        int ia = unit_cell_.spl_num_atoms(ialoc);
        auto& atom_type = unit_cell_.atom(ia).type();

        int nmtp = atom_type.num_mt_points();
        int num_rf_pairs = atom_type.mt_radial_basis_size() * (atom_type.mt_radial_basis_size() + 1) / 2;

        utils::timer t1("sirius::Density::generate|sum_zdens");
        switch (ctx_.num_mag_dims()) {
            case 3: {
                reduce_density_matrix<3>(atom_type, ia, density_matrix_, *gaunt_coefs_, mt_density_matrix);
                break;
            }
            case 1: {
                reduce_density_matrix<1>(atom_type, ia, density_matrix_, *gaunt_coefs_, mt_density_matrix);
                break;
            }
            case 0: {
                reduce_density_matrix<0>(atom_type, ia, density_matrix_, *gaunt_coefs_, mt_density_matrix);
                break;
            }
        }
        t1.stop();

        utils::timer t2("sirius::Density::generate|expand_lm");
        /* collect radial functions */
        for (int idxrf2 = 0; idxrf2 < atom_type.mt_radial_basis_size(); idxrf2++) {
            int offs = idxrf2 * (idxrf2 + 1) / 2;
            for (int idxrf1 = 0; idxrf1 <= idxrf2; idxrf1++) {
                /* off-diagonal pairs are taken two times: d_{12}*f_1*f_2 + d_{21}*f_2*f_1 = d_{12}*2*f_1*f_2 */
                int n = (idxrf1 == idxrf2) ? 1 : 2;
                for (int ir = 0; ir < unit_cell_.atom(ia).num_mt_points(); ir++) {
                    rf_pairs(ir, offs + idxrf1) = n * unit_cell_.atom(ia).symmetry_class().radial_function(ir, idxrf1) *
                                                      unit_cell_.atom(ia).symmetry_class().radial_function(ir, idxrf2);
                }
            }
        }
        for (int j = 0; j < ctx_.num_mag_dims() + 1; j++) {
            linalg<device_t::CPU>::gemm(0, 1, ctx_.lmmax_rho(), nmtp, num_rf_pairs,
                                        &mt_density_matrix(0, 0, j), mt_density_matrix.ld(),
                                        &rf_pairs(0, 0), rf_pairs.ld(), &dlm(0, 0, j), dlm.ld());
        }

        int sz = static_cast<int>(ctx_.lmmax_rho() * nmtp * sizeof(double));
        switch (ctx_.num_mag_dims()) {
            case 3: {
                std::memcpy(&magnetization(1).f_mt<index_domain_t::local>(0, 0, ialoc), &dlm(0, 0, 2), sz);
                std::memcpy(&magnetization(2).f_mt<index_domain_t::local>(0, 0, ialoc), &dlm(0, 0, 3), sz);
            }
            case 1: {
                for (int ir = 0; ir < nmtp; ir++) {
                    for (int lm = 0; lm < ctx_.lmmax_rho(); lm++) {
                        rho().f_mt<index_domain_t::local>(lm, ir, ialoc) = dlm(lm, ir, 0) + dlm(lm, ir, 1);
                        magnetization(0).f_mt<index_domain_t::local>(lm, ir, ialoc) = dlm(lm, ir, 0) - dlm(lm, ir, 1);
                    }
                }
                break;
            }
            case 0: {
                std::memcpy(&rho().f_mt<index_domain_t::local>(0, 0, ialoc), &dlm(0, 0, 0), sz);
            }
        }
    }
}

void Density::symmetrize_density_matrix()
{
    PROFILE("sirius::Density::symmetrize_density_matrix");

    auto& sym = unit_cell_.symmetry();

    int ndm = ctx_.num_mag_comp();

    mdarray<double_complex, 4> dm(unit_cell_.max_mt_basis_size(),
                                  unit_cell_.max_mt_basis_size(),
                                  ndm,
                                  unit_cell_.num_atoms());
    dm.zero();

    int lmax  = unit_cell_.lmax();
    int lmmax = utils::lmmax(lmax);
    mdarray<double, 2> rotm(lmmax, lmmax);

    for (int i = 0; i < sym.num_mag_sym(); i++) {
        int  pr   = sym.magnetic_group_symmetry(i).spg_op.proper;
        auto eang = sym.magnetic_group_symmetry(i).spg_op.euler_angles;
        int  isym = sym.magnetic_group_symmetry(i).isym;
        SHT::rotation_matrix(lmax, eang, pr, rotm);
        auto spin_rot_su2 = rotation_matrix_su2(sym.magnetic_group_symmetry(i).spin_rotation);

        for (int ia = 0; ia < unit_cell_.num_atoms(); ia++) {
            int ja = sym.sym_table(ia, isym);

            sirius::symmetrize(density_matrix_, unit_cell_.atom(ia).type().indexb(), ia, ja, ndm, rotm,
                               spin_rot_su2, dm, false);
        }
    }

    double alpha = 1.0 / double(sym.num_mag_sym());
    /* multiply by alpha which is the inverse of the number of symmetries */
    auto a = dm.at(memory_t::host);
    for (auto i = 0u; i < dm.size(); i++) {
        a[i] *= alpha;
    }

    dm >> density_matrix_;

    if (ctx_.control().print_checksum_ && ctx_.comm().rank() == 0) {
        auto cs = dm.checksum();
        utils::print_checksum("density_matrix", cs);
        //for (int ia = 0; ia < unit_cell_.num_atoms(); ia++) {
        //    auto cs = mdarray<double_complex, 1>(&dm(0, 0, 0, ia), dm.size(0) * dm.size(1) * dm.size(2)).checksum();
        //    DUMP("checksum(density_matrix(%i)): %20.14f %20.14f", ia, cs.real(), cs.imag());
        //}
    }

    if (ctx_.control().print_hash_ && ctx_.comm().rank() == 0) {
        auto h = dm.hash();
        utils::print_hash("density_matrix", h);
    }
}

mdarray<double, 2>
Density::compute_atomic_mag_mom() const
{
    PROFILE("sirius::Density::compute_atomic_mag_mom");

    mdarray<double, 2> mmom(3, unit_cell_.num_atoms());
    mmom.zero();

    #pragma omp parallel for
    for (int ia = 0; ia < unit_cell_.num_atoms(); ia++) {

        auto& atom_to_grid_map = ctx_.atoms_to_grid_idx_map(ia);

        for (auto coord : atom_to_grid_map) {
            int ir = coord.first;
            for (int j = 0; j < ctx_.num_mag_dims(); j++) {
                mmom(j, ia) += magnetization(j).f_rg(ir);
            }
        }

        for (int j : {0, 1, 2}) {
            mmom(j, ia) *= (unit_cell_.omega() / ctx_.fft().size());
        }
    }
    ctx_.fft().comm().allreduce(&mmom(0, 0), static_cast<int>(mmom.size()));
    return mmom;
}

mdarray<double, 3>
Density::density_matrix_aux(int iat__)
{
    auto& atom_type = unit_cell_.atom_type(iat__);
    int   nbf       = atom_type.mt_basis_size();

    /* convert to real matrix */
    mdarray<double, 3> dm(nbf * (nbf + 1) / 2, atom_type.num_atoms(), ctx_.num_mag_dims() + 1);
    #pragma omp parallel for
    for (int i = 0; i < atom_type.num_atoms(); i++) {
        int ia = atom_type.atom_id(i);

        for (int xi2 = 0; xi2 < nbf; xi2++) {
            for (int xi1 = 0; xi1 <= xi2; xi1++) {
                int idx12 = xi2 * (xi2 + 1) / 2 + xi1;
                switch (ctx_.num_mag_dims()) {
                    case 3: {
                        dm(idx12, i, 2) = 2 * std::real(density_matrix_(xi2, xi1, 2, ia));
                        dm(idx12, i, 3) = -2 * std::imag(density_matrix_(xi2, xi1, 2, ia));
                    }
                    case 1: {
                        dm(idx12, i, 0) = std::real(density_matrix_(xi2, xi1, 0, ia) + density_matrix_(xi2, xi1, 1, ia));
                        dm(idx12, i, 1) = std::real(density_matrix_(xi2, xi1, 0, ia) - density_matrix_(xi2, xi1, 1, ia));
                        break;
                    }
                    case 0: {
                        dm(idx12, i, 0) = density_matrix_(xi2, xi1, 0, ia).real();
                        break;
                    }
                }
            }
        }
    }
    return dm;
}

} // namespace sirius