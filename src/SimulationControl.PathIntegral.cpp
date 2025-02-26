#include <algorithm>
#include <cstring>
#include <iostream>
#include <map>
#include <time.h>
#include <vector>

#include "Output.h"
#include "Quaternion.h"
#include "SafeOps.h"
#include "SimulationControl.h"
#include "Vector3D.h"

#ifdef _MPI
#include <mpi.h>
#endif
extern int rank, size;
extern bool mpi;





bool SimulationControl::PI_nvt_mc() {

	typedef struct  BFC {
		double init;
		double trial;
		double current;
		inline double change() { return trial - init; }
	} BoltzFactor_contributor;
	BoltzFactor_contributor energy = { 0, 0, 0 };
	BoltzFactor_contributor PI_chainlength = { 0, 0, 0 };
	BoltzFactor_contributor PI_orient_dist = { 0, 0, 0 };
	
	System *system = systems[rank]; // System for *this* MPI thread
	int nSteps = system->numsteps;  // number of MC steps to perform
	int move = 0;                   // current MC move 

	if (mpi) {
		system->setup_mpi_dataStructs();
	}
	else std::for_each(systems.begin(), systems.end(), [this](System *SYS) {
		SYS->setup_mpi_dataStructs();
	});

	
	std::for_each( systems.begin(), systems.end(), [](System *SYS) {
		if(SYS->cavity_bias)
			SYS->cavity_update_grid(); // update the grid for the first time 
		SYS->observables->volume = SYS->pbc.volume; // set volume observable
	});

	// If starting from scratch, so an initial bead-perturbation and compute the energy 
	if( ! sys.parallel_restarts )
		PI_perturb_bead_COMs_ENTIRE_SYSTEM(1.0);
	energy.init = PI_system_energy();
	
	// solve for the rotational energy levels 
	//if (systems[rank]->quantum_rotation) quantum_system_rotational_energies(systems[rank]);
	
	// be a bit forgiving of the initial state 
	if( ! std::isfinite(energy.init) )
		energy.init = MAXVALUE;
	
	//|  write initial observables to stdout and logs
	//|  THIS SECTION MAY NEED TO BE DONE FOR EACH SYSTEM INDIVIDUALLY---NOT SURE WHAT WE WANT YET
	//|________________________________________________________________________________________________________________________|
	if(  ! rank  ) {
		system->open_files(); // open output files 
		system->calc_system_mass();
		// average in the initial values once  (we don't want to double-count the initial state when using MPI)
		system->update_root_averages(system->observables);
		// write initial observables exactly once
		if (system->fp_energy)
			system->write_observables(system->fp_energy, system->observables, system->temperature);
		if (system->fp_energy_csv)
			system->write_observables_csv(system->fp_energy_csv, system->observables, system->temperature);
		Output::out("MC: initial values:\n");
		system->write_averages();
	}


	//  save the initial state 
	System::backup_observables( systems );
	move = PI_pick_NVT_move();


	// main MC loop 
	for( sys.step=1; sys.step <= nSteps; sys.step++ ) {

		// update step for each system. 
		for (int i = 0; i < PI_nBeads; i++) 
			systems[i]->step = sys.step;

		// restore the last accepted energy & chain-length-metric
		energy.init = PI_observable_energy();
		if (move == MOVETYPE_PERTURB_BEADS)
			PI_kinetic_E( PI_chainlength.init, PI_orient_dist.init );

		// perturb the system 
		PI_make_move( move );

		// calculate new energy change, new PI chain length & update obervables
		energy.trial = PI_system_energy();
		if (move == MOVETYPE_PERTURB_BEADS)
			PI_kinetic_E( PI_chainlength.trial, PI_orient_dist.trial );

		#ifdef QM_ROTATION
			// solve for the rotational energy levels 
			if (system->quantum_rotation && (system->checkpoint->movetype == MOVETYPE_SPINFLIP))
				quantum_system_rotational_energies(system);
		#endif // QM_ROTATION 

		// treat a bad contact as a reject 
		if( ! std::isfinite(energy.trial) ) {
			system->observables->energy = MAXVALUE;
			system->nodestats->boltzmann_factor = 0;
		} else 
			PI_NVT_boltzmann_factor( energy.change(), PI_chainlength.change(), PI_orient_dist.change() );

			
		
		// Metropolis function 
		///////////////////////////////////////////////////////////////////////////////////////////////////////////////

		if(   (Rando::rand() < system->nodestats->boltzmann_factor)   &&   (system->iterator_failed == 0)   ) {
			  
			 //  ACCEPT  //////////////////////////////////////////////////////////////////////////////////////////////
			if (reportAR()) {
				Output::out1("ACCEPT               ");
				switch (move) {
				case MOVETYPE_DISPLACE:      Output::out1("Displace\n"  ); break;
				case MOVETYPE_PERTURB_BEADS: Output::out1("Bead reorg\n"); break;
			}}


			energy.current = energy.trial;
			PI_chainlength.current = PI_chainlength.trial;
			PI_orient_dist.current = PI_orient_dist.trial;

			System::backup_observables(systems);

			move = PI_pick_NVT_move();
			system->register_accept();

			// Simulated Annealing
			if (sys.simulated_annealing) {
				if (sys.simulated_annealing_linear)	{
					system->temperature = system->temperature + (system->simulated_annealing_target - system->temperature) / (nSteps - system->step);
					if (nSteps == system->step)
						systems[rank]->temperature = systems[rank]->simulated_annealing_target;
				}
				else
					systems[rank]->temperature = systems[rank]->simulated_annealing_target + (systems[rank]->temperature - systems[rank]->simulated_annealing_target)*systems[rank]->simulated_annealing_schedule;
			}

		} else {

			//  REJECT: restore from last checkpoint  /////////////////////////////////////////////////////////////////
			if (reportAR()) {
				Output::out1("REJECT   ");
				switch (move) {
				case MOVETYPE_DISPLACE:      Output::out1("Displace\n"  ); break;
				case MOVETYPE_PERTURB_BEADS: Output::out1("Bead reorg\n"); break;
				}
			}
			energy.current = energy.trial; //used in parallel tempering
			for_each( systems.begin(), systems.end(), [](System *SYS) {
				SYS->iterator_failed = 0; // reset the polar iterative failure flag
				SYS->restore();            
				SYS->register_reject();
			});
			move = PI_pick_NVT_move();
		}



		// perform parallel_tempering
		if ((systems[rank]->parallel_tempering) && ((system->step % systems[rank]->ptemp_freq) == 0))
			systems[rank]->temper_system(energy.current);

		// track the acceptance_rate 
		systems[rank]->track_ar(systems[rank]->nodestats);

		// each node calculates its stats 
		systems[rank]->update_nodestats(systems[rank]->nodestats, systems[rank]->avg_nodestats);


		// do this every correlation time, and at the very end ////////////////////////////////////////////////////////
		if (!(system->step % system->corrtime) || (system->step == nSteps)) 
			do_PI_corrtime_bookkeeping();

		if (system->step == nSteps) {
			for( int i=0; i<size; i++ ) {
				#ifdef _MPI
					MPI_Barrier(MPI_COMM_WORLD);
				#endif
				if( i==rank )
					write_PI_frame();
		}}

	} // main loop 


	for_each(systems.begin(), systems.end(), [](System *SYS) {
		if (SYS->write_molecules_wrapper( SYS->pqr_output ) < 0) {
			Output::err("MC: could not write final state to disk\n");
			throw unknown_file_error;
		}
	});


	return ok;
}




void SimulationControl::do_PI_corrtime_bookkeeping() {

	System *system = systems[rank];
	
	if( !rank && PI_xyz_corrtime_frames_requested()) 
		write_PI_frame();


	//  if (sys.calc_hist) {
	//  	system->zero_grid(sys.grids->histogram->grid);
	//  	system->population_histogram();
	//  }


	// copy observables and avgs to the mpi send buffer
	// histogram array is at the end of the message
	std::for_each(systems.begin(), systems.end(), [](System *SYS) {
		if (SYS->calc_hist) {
			SYS->zero_grid(SYS->grids->histogram->grid);
			SYS->population_histogram();
		}

		// update frozen and total system mass
		SYS->calc_system_mass();
		//system->calc_system_mass();

		// update sorbate info on each node
		if (SYS->sorbateCount > 1)
			SYS->update_sorbate_info();
		//system->update_sorbate_info();


	});


	
	if (mpi) {

		
		for (int j = 0; j < size; j++) {
			#ifdef _MPI
				MPI_Barrier(MPI_COMM_WORLD);
			#endif
			// Write output files, one at a time to avoid disk congestion
			if (j == rank) {

				// write trajectory files for each node
				system->write_states();

				// write restart files for each node
				if (system->write_molecules_wrapper(system->pqr_restart) < 0) {
					Output::err("MC: could not write restart state to disk\n");
					throw unknown_file_error;
				}

				// dipole/field data for each node
				if (sys.polarization) {
					systems[j]->write_dipole();
					systems[j]->write_field();
				}
			}
		}
	}
	else {

		bool energy_written = false;
		std::for_each(systems.begin(), systems.end(), [&energy_written](System *SYS) {

			if( ! energy_written ) {
				SYS->write_states();
				energy_written = true;
			}

			// write restart files for each system
			if (SYS->write_molecules_wrapper(SYS->pqr_restart) < 0) {
				Output::err("MC: could not write restart state to disk\n");
				throw unknown_file_error;
			}

			// write dipole/field data for each system
			if (SYS->polarization) {
				SYS->write_dipole();
				SYS->write_field();
			}
		});
	}


	if (mpi) {
		// zero the send buffer
		std::memset(system->mpi_data.snd_strct, 0, system->mpi_data.msgsize);
		std::memcpy(system->mpi_data.snd_strct, system->observables, sizeof(System::observables_t));
		std::memcpy(system->mpi_data.snd_strct + sizeof(System::observables_t), system->avg_nodestats, sizeof(System::avg_nodestats_t));

		if (sys.calc_hist)
			system->mpi_copy_histogram_to_sendbuffer(
				system->mpi_data.snd_strct + sizeof(System::observables_t) + sizeof(System::avg_nodestats_t),
				system->grids->histogram->grid
			);
		if (sys.sorbateCount > 1)
			std::memcpy(
				system->mpi_data.snd_strct + sizeof(System::observables_t) + sizeof(System::avg_nodestats_t) + system->calc_hist * system->n_histogram_bins * sizeof(int), //compensate for the size of hist data, if neccessary
				system->sorbateInfo,
				system->sorbateCount * sizeof(System::sorbateInfo_t)
			);
	} 
	else std::for_each(systems.begin(), systems.end(), [](System *SYS) {
		
		// zero the send buffer
		std::memset(SYS->mpi_data.snd_strct, 0, SYS->mpi_data.msgsize);
		std::memcpy(SYS->mpi_data.snd_strct, SYS->observables, sizeof(System::observables_t));
		std::memcpy(SYS->mpi_data.snd_strct + sizeof(System::observables_t), SYS->avg_nodestats, sizeof(System::avg_nodestats_t));

		if (SYS->calc_hist)
			SYS->mpi_copy_histogram_to_sendbuffer(
				SYS->mpi_data.snd_strct + sizeof(System::observables_t) + sizeof(System::avg_nodestats_t),
				SYS->grids->histogram->grid
			);
		if (SYS->sorbateCount > 1)
			std::memcpy(
				SYS->mpi_data.snd_strct + sizeof(System::observables_t) + sizeof(System::avg_nodestats_t) + SYS->calc_hist * SYS->n_histogram_bins * sizeof(int), //compensate for the size of hist data, if neccessary
				SYS->sorbateInfo,
				SYS->sorbateCount * sizeof(System::sorbateInfo_t)
			);
	});
	
	
	if( ! rank )
		std::memset(system->mpi_data.rcv_strct, 0, size * system->mpi_data.msgsize);

	if (mpi) {
		#ifdef _MPI
			// copy all data into the receive struct of the head node...
			MPI_Gather( system->mpi_data.snd_strct, 1, system->msgtype, system->mpi_data.rcv_strct, 1, system->msgtype, 0, MPI_COMM_WORLD);
			MPI_Gather( &(system->temperature), 1, MPI_DOUBLE, system->mpi_data.temperature, 1, MPI_DOUBLE, 0, MPI_COMM_WORLD);
		#endif
	} else {
		for (int i = 0; i < systems.size(); i++) {
			// ...or if single-threaded, copy data from the each of the send structs into the appropriate part of the receive struct manually
			std::memcpy( system->mpi_data.rcv_strct + i * system->mpi_data.msgsize, systems[i]->mpi_data.snd_strct, system->mpi_data.msgsize);
			system->mpi_data.temperature[i] = systems[i]->temperature;
		}
	}

	// head node collects all observables and averages
	if( ! rank ) {

		// clear avg_nodestats to avoid double-counting
		system->clear_avg_nodestats();

		//loop for each core -> shift data into variable_mpi, then average into avg_observables
		for( int j = 0; j < size; j++ ) {
			// copy from the mpi buffer
			std::memcpy( systems[0]->mpi_data.observables,   systems[0]->mpi_data.rcv_strct  +  j * systems[0]->mpi_data.msgsize,  sizeof(System::observables_t )); // copy observable data into observables struct, for one system at a time
			std::memcpy( systems[0]->mpi_data.avg_nodestats, systems[0]->mpi_data.rcv_strct  +  j * systems[0]->mpi_data.msgsize + sizeof(System::observables_t), sizeof(System::avg_nodestats_t )); // ditto for avg_nodestates

			if( sys.calc_hist )
				system->mpi_copy_rcv_histogram_to_data( systems[0]->mpi_data.rcv_strct  +  j * systems[0]->mpi_data.msgsize  +  sizeof(System::observables_t) + sizeof(System::avg_nodestats_t), systems[0]->grids->histogram->grid ); // ditto for histogram grid
			if( sys.sorbateCount > 1 )
				std::memcpy( systems[0]->mpi_data.sinfo, // ditto for sorbateInfo
					systems[0]->mpi_data.rcv_strct   +   j * systems[0]->mpi_data.msgsize   +   sizeof(System::observables_t)   +   sizeof(System::avg_nodestats_t)   +   sys.calc_hist * sys.n_histogram_bins * sizeof(int), //compensate for the size of hist data, if neccessary
					sys.sorbateCount * sizeof(System::sorbateInfo_t)
				);
			
			// write observables
			if( system->fp_energy )
				system->write_observables( system->fp_energy, system->mpi_data.observables, system->mpi_data.temperature[j] );
			if( system->fp_energy_csv )
				system->write_observables_csv( system->fp_energy_csv, system->mpi_data.observables, system->mpi_data.temperature[j] );
			if (system->fp_xyz)
			{
				system->write_molecules_xyz(system->fp_xyz); //L
			}

			// collect the averages
			// if parallel tempering, we will collect obserables from the coldest bath. this can't be done for nodestats though,
			// since nodestats are averaged over each corrtime, rather than based on a single sample taken at the corrtime
			system->update_root_nodestats( system->mpi_data.avg_nodestats, system->avg_observables );
			
			if ( ! sys.parallel_tempering ) {
				/////////////////////////////////////////////////////////////////////////////////////////////
				// This is what needs to be updated for PI functionality:   /////////////////////////////////
				system->update_root_averages(system->mpi_data.observables); // system->avg_observables );
				
				if( sys.calc_hist )
					system->update_root_histogram();
				if( sys.sorbateCount > 1 )
					system->update_root_sorb_averages( system->mpi_data.sinfo );
				/////////////////////////////////////////////////////////////////////////////////////////////
				/////////////////////////////////////////////////////////////////////////////////////////////
			}
			else if ( system->ptemp->index[j] == 0 ) {
				system->update_root_averages( system->mpi_data.observables ); // , system->avg_observables );
				if( sys.calc_hist )
					system->update_root_histogram();
				if( sys.sorbateCount > 1 )
					system->update_root_sorb_averages( system->mpi_data.sinfo );
			}
		}

		system->output_file_data();

	} // !rank
}




double SimulationControl::PI_NVT_boltzmann_factor( double delta_energy, double delta_chain2, double delta_orient2) {

	const int     P = (int)systems.size(); // Trotter number: number of beads (or "systems") in PI representation
	const double  T = sys.temperature;
	double        boltzmann_factor = 0;


	switch (systems[rank]->checkpoint->movetype) {

	case MOVETYPE_PERTURB_BEADS:
	{
		

		double mol_mass = systems[rank]->checkpoint->molecule_altered->mass * AMU2KG; // mass of the molecule (in kg)
		double mLambda2 = (h*h) / (2.0 * pi * mol_mass * kB * T); // thermal wavelength squared
		//double red_mass = 0; // reduced mass of the perturbed molecule
		//double uLambda2 = 0; // thermal wavelength squared, reduced mass
		double energy_contrib = delta_energy / T; // potential energy contribution
		double PI_COM_contrib = 0; // PI COM energy contribution
		double PI_orientation_contrib = 0; // PI orientational energy contribution
		double reduced_mass = 0;  // reduced mass of the perturbed molecule
		double uLambda2 = 0;  // thermal wavelength squared of reduced mass

		// calculate a boltzmann factor for a bead perturbation
		std::map<std::string, int>::iterator it;
		it = sorbate_data_index.find(systems[rank]->checkpoint->molecule_altered->moleculetype);
		if (it == sorbate_data_index.end()) {

			// COM-only case (sorbate metadata not found)
			// the chain lengths of all the molecules should cancel out, except for the one that was moved (molecule_altered)
			// and it is the mass of this molecule that we use in the PI_COM_contrib calculation (ditto for 'else' case)
			PI_COM_contrib = PI_COM_contrib = delta_chain2 * pi * P / mLambda2;
			PI_orientation_contrib = 0.0;
			
		} else {

			// COM + Orientation PI case
			PI_COM_contrib = delta_chain2 * pi * P / mLambda2;
			reduced_mass = sorbate_data[it->second].reduced_mass;
			uLambda2 = (h*h) / (2.0 * pi * reduced_mass * kB * T);
			PI_orientation_contrib = delta_orient2 * pi * P / uLambda2;
		}

		boltzmann_factor = exp(-energy_contrib - PI_COM_contrib - PI_orientation_contrib);
	}
	break;

	case MOVETYPE_SPINFLIP:
	{
		double partfunc_ratio = 0;
		double g = systems[rank]->checkpoint->molecule_altered->rot_partfunc_g;
		double u = systems[rank]->checkpoint->molecule_altered->rot_partfunc_u;
		if (systems[rank]->checkpoint->molecule_altered->nuclear_spin == NUCLEAR_SPIN_PARA)
			partfunc_ratio = g / (g + u);
		else
			partfunc_ratio = u / (g + u);

		// set the boltz factor, including ratio of partfuncs for different symmetry rotational levels
		boltzmann_factor = partfunc_ratio;
	}
	break;

	default: // DISPLACE
		boltzmann_factor = exp(-delta_energy / sys.temperature);

	}
	systems[rank]->nodestats->boltzmann_factor = boltzmann_factor;

	return boltzmann_factor;
}




bool SimulationControl::check_PI_options() {
	
	//  Check to see if the bead count (i.e. MPI size) is a power of 2
	//  (i.e. count the number of ones in the binary representation of 'size' and make sure there is only 1)

	unsigned int bits = sizeof(size) * 8;
	unsigned int bitcount = 0;
	unsigned long long int bitmask = 1; 
	char linebuf[maxLine];

	for( unsigned int i = 0; i < bits; i++ ) {
		if( PI_nBeads & bitmask )
			bitcount++;
		bitmask = bitmask << 1;
	}

	if(  (PI_nBeads<4) || (bitcount != 1)  ) {
		sprintf(linebuf, "SIMULATION CONTROL: MPI-Reported world-size: %d.\n", size );
		Output::out("SIMULATION CONTROL: Path Integrals require at least 4 MPI processes to run. One process per PI bead and a total of 2^N 'beads' (N >= 2).\n");
		Output::err(linebuf);
		throw invalid_MPI_size_for_PI;
	}

	// Make sure the desired length of trial chains for bead perturbations is set appropriately

	if( ! PI_trial_chain_length ) {
		Output::err("SIMULATION CONTROL: PI_trial_chain_length must be set when using Path Integral ensembles.\n");
		throw invalid_setting;
	}
	if( (PI_trial_chain_length < 0)  ||  (PI_trial_chain_length >= PI_nBeads) ){
		Output::err( "SIM_CONTROL: PI_trial_chain_length must be in [1..P-1], where P is the Trotter number,\n"     );
		Output::err( "             i.e. the number of 'beads' (1 bead per MPI thread). For a single (non-MPI)\n"    );
		Output::err( "             thread, bead count is set with the 'trotter_number' option in the input file.\n" );
		if(mpi) 
			sprintf(linebuf, "SIM_CONTROL: MPI-Reported world-size (PI bead count): %d.\n", size);
		else 
			sprintf(linebuf, "SIM_CONTROL: user requested bead count (single thread): %d\n", PI_nBeads );
		Output::err(linebuf);
		sprintf(linebuf, "SIM_CONTROL: requested length of trial chain: %d.\n", PI_trial_chain_length);
		Output::err(linebuf);
		throw invalid_setting;
	}
	
	return ok;
}




void SimulationControl::initialize_PI_NVT_Systems() {

	char linebuf[maxLine] = {0};
	Output::out1("\n");

	// Create a system object for each bead on the Path Integral loop, and populate those systems with the system geometry. 

	
	for( int i=0; i<PI_nBeads; i++ ) {
		
		systems.push_back( new System(sys) );
		
		if( sys.parallel_restarts)
			strcpy(systems[i]->pqr_input, pqr_input_filenames[i].c_str());
		strcpy(systems[i]->pqr_output,    pqr_final_filenames[i].c_str());
		strcpy(systems[i]->pqr_restart, pqr_restart_filenames[i].c_str());
		sprintf(linebuf, "SIM_CONTROL: SYSTEM[ %d ] Instantiated.\nSIM_CONTROL->SYSTEM[ %d ]: Constructing simulation box.\n", i, i );
		Output::out1(linebuf);
		
		systems[i]->setup_simulation_box();
		sprintf(linebuf, "SIM_CONTROL->SYSTEM[ %d ]: simulation box configured.\n\n", i);
		Output::out1(linebuf);
	
		systems[i]->allocateStatisticsMem();

		if (systems[i]->calc_hist)
			systems[i]->setup_histogram();

		if (systems[i]->cavity_bias)
			systems[i]->setup_cavity_grid();

		// ensure that all SPECTRE charges lie within the restricted domain 
		if (systems[i]->spectre)
			systems[i]->spectre_wrapall();

		if (mpi && (i != rank)) continue;
		// The initialization that follows supports energy calculations, which will only be done on 
		// rank's system (for multi-threaded runs) and so will only be instantiated on rank's data
		// structure. For single threaded runs these features will need to be enabled on all systems.
		///////////////////////////////////////////////////////////////////////////////////////////////////
		

		// Set up the pairs list etc. This must happen for every system on a single thread, but only for the system (i.e. 
		// systems[rank]) that this controller will be manipulating & for which it will be performing energy computations. 
		systems[i]->allocate_pair_lists();
		systems[i]->pairs();          // get all of the pairwise interactions, exclusions, etc.
		systems[i]->flag_all_pairs(); // set all pairs to initially have their energies calculated 
		
		
		// if polarization active, allocate the necessary matrices 
		if(  systems[i]->polarization  &&  ( ! systems[i]->cuda )  &&  ( ! systems[i]->polar_zodid )  )
			systems[i]->thole_resize_matrices();		
	}

	if (mpi) {
		#ifdef _MPI
			MPI_Barrier(MPI_COMM_WORLD);
		#endif	
	}

	Output::out1("SIM_CONTROL: finished allocating pair lists\n");
	Output::out1("SIM_CONTROL: finished calculating pairwise interactions\n");
	if( ! sys.use_sg || sys.rd_only ) {
		sprintf(linebuf, "SIM_CONTROL: Ewald gaussian width = %f A\n", sys.ewald_alpha);
		Output::out1(linebuf);
		sprintf(linebuf, "SIM_CONTROL: Ewald kmax = %d\n", sys.ewald_kmax);
		Output::out1(linebuf);
	}
}




void SimulationControl::write_PI_frame() {
// output an XYZ format frame for all systems

	const int very_first_frame_number = 1;

	int nSys   = (int) systems.size();
	int nSites = nSys * systems[0]->countNatoms();

	static int frame_number = very_first_frame_number;
	char write_mode[2] = { 'w', '\0' };
	if( frame_number == very_first_frame_number )
		write_mode[0] = 'w';

	char filename[500];
	


	for (int i = 0; i < size; i++) {
		
		if (i == rank) {

			sprintf(filename, "frames.%d.xyz", rank);
			FILE *outFile = fopen( filename, write_mode );

			fprintf(outFile, "%d\nFrame: %d\n", nSites, frame_number);

			++frame_number;

			Molecule * m;
			Atom * a;
			for (int s = 0; s < nSys; s++) {
				for (m = systems[s]->molecules; m; m = m->next) {
					for (a = m->atoms; a; a = a->next) {
						fprintf(outFile, "%s     %0.4lf     %0.4lf     %0.4lf\n", a->atomtype, a->pos[0], a->pos[1], a->pos[2]);
					}
				}
			}

			fclose(outFile);
		}
	}
}




double SimulationControl::PI_system_energy() {

	static bool first_run = true;
	
	if (first_run) {
		first_run = false;
		SafeOps::calloc(system_energies, PI_nBeads, sizeof(double), __LINE__, __FILE__); // free'd in ~SimulationControl();
	}
	double energy = 0;
	

	if( mpi ) {
		energy = systems[rank]->energy();
		#ifdef _MPI
			MPI_Allgather( &energy, 1, MPI_DOUBLE, system_energies, 1, MPI_DOUBLE, MPI_COMM_WORLD);
		#endif
		energy = 0;
		for (int i = 0; i < PI_nBeads; i++) {
			systems[i]->observables->energy = system_energies[i];
			energy += system_energies[i];
		}

		// Populate these, via allgather, if needed:
		// systems[i]->observables->coulombic_energy = "allgathered data";
		// Ditto for (all are quantities updated in energy()):
		// observables->kinetic_energy, observables->coulombic_energy, observables->polarization_energy
		// observables->vdw_energy, observables->rd_energy, observables->three_body_energy, observables->spin_ratio
		// observables->kinetic_energy (NVE), observables->temperature (NVE), observables->NU

		return energy / PI_nBeads;
	}


	// For single-threaded systems, energy computations happen on every system
	// This is done in two passes so that energy values will be populated in all systems...
	for( int s=0; s < PI_nBeads; s++ )
		system_energies[s] = systems[s]->energy();

	// ...and on the second pass energies are summed and checked for infinite values.
	for( int s=0; s < PI_nBeads; s++) {

		double E = system_energies[s];
		if( ! std::isfinite(E) ) 
			return E; // infinite energy short-circuits the sum
		else
			energy += E;
	}

	return energy/PI_nBeads;
}




double SimulationControl::PI_observable_energy() {

	double energy = 0;
	int nSystems = (int)systems.size();
	for (int s = 0; s < nSystems; s++) 
		energy += systems[s]->observables->energy;
	
	return energy / (double) nSystems;
}




void SimulationControl::PI_kinetic_E(double &bead_separation, double &orientation_difference) {

	// Ensure that a molecule has been targeted for perturbation.
	for_each(systems.begin(), systems.end(), [](System *s) {
		if (s->checkpoint->molecule_altered == nullptr) {
			Output::err("System corrupted. No target molecule specified for PI chain measurement request.");
			throw internal_error;
		}
	});

	// Take the measurements
	bead_separation = PI_chain_length();
	orientation_difference = PI_orientational_distance();
}




double SimulationControl::PI_chain_length() {

	// WARNING: This function only computes the chain length of the molecule targeted for perturbation (as the lengths
	// of all non-changing molecule PI chains will cancel in the Boltzmann factor. As such, if no target is specified,
	// something has gone wrong.

	const int     P = (int)systems.size();  // Trotter number: number of beads (or "systems") in PI representation
	
	// To compute the weighted length of the "polymer chain", we must compute a harmonic-well-type potential between adjacent images
	// of congruent molecular species. Systems 1 -> P should all have the same number and type of molecular constiutents. Molecule 1
	// in System 1 is a partial representation of Molecule 1. Molecule 1 in System 2 is another part of that representation and each
	// system can be thought of as a "many worlds" representation where each member exists in every system but is affected by slightly
	// different circumstances. The complete representation of Molecule 1 is an average over all the "Molecule 1"'s in every system.
	// The "polymer chain" of constituent beads for each molecule are connected by a harmonic potential between each of them that makes
	// said images form a coherent representation of a single particle and prevents the beads from drifting apart independently of each
	// other. The PI "chain length" for a single molecule is a loop of harmonic "potentials" between the COMs of the adjacent molecule
	// images that together constitute the representation of that molecule. E.g.:
	// Mol1COM,Sys1  \___/  Mol1COM,Sys2  \___/  Mol1COM,Sys3  \___/  Mol1COM,Sys1 (and here we are back at the first system)
	

	double        PI_chain_length = 0;      // "length", for lack of a better word. It is the mass*(distance^2 + distance^2 + ...) for the
	                                        // molecule whose bead representation was perturbed. The distances measure the separations 
	                                        // between adjacent bead COMs on the PI "polymer chain". It is actually some sort of weighted
	                                        // length quantity, but it plays the same role as energy if we were simulating a harmonic potential.


	std::vector<Molecule *> molecules;      // A vector of molecule pointers to index the rep of the same molecule in each system
	std::vector<Vector3D> COMs;
	
	for_each(systems.begin(), systems.end(), [&molecules](System *s) {
		molecules.push_back(s->checkpoint->molecule_altered);
	});

	// record the COM coordinates of each system's version of the target molecule.
	for_each(molecules.begin(), molecules.end(), [&COMs](Molecule *m) {
		m->update_COM();
		Vector3D com(m->com[0], m->com[1], m->com[2]);
		COMs.push_back(com);
	});
	
	// Now, 'molecules' points to the respective version of the same molecule in each of the P systems. We have the COM
	// coords of each different image of that molecule recorded in x,y,z; so we effectively have the coords of the 'loop'
	// representation of a single molecule in the x,y,z arrays. The loop being formed in the manner, e.g.:
	//
	// x[0] <~> x[1] <~> x[2] <~> x[0]      |  where ( x[0], y[0], z[0] ) is the COM position for the current molecule in
	// y[0] <~> y[1] <~> y[2] <~> y[0]      |  the first system. And where the COM position for the SAME molecule in the 
	// z[0] <~> z[1] <~> z[2] <~> z[0]      |  second system is ( x[1], y[1], z[1] ).

	// Cycle around the COM coordinate loop and sum the squared distance for each adjacent pair, i.e. we will be 
	// finding dist^2 values for each pair of coordinates (x[i],y[i],z[i]) & (x[i+1], y[i+1], z[i+1]), such that the
	// last xyz coords in the list will be paired with the first.

	for (int i = 0; i < P; i++) {
		int j = (i + 1) % P;
		Vector3D delta = COMs[i] - COMs[j];
		PI_chain_length += delta.norm2();
	}
	PI_chain_length *= (ANGSTROM2METER * ANGSTROM2METER); // convert A^2 to m^2

	return PI_chain_length;
}




double SimulationControl::PI_orientational_distance() {
// If each bond is described as a vector, this function computes the difference vector between bonds on adjacent molecules in the PI
// bead chain. It takes the squared-norm of all these differences and returns the their sum. IT ONLY COMPUTES this difference on the
// the molecule targeted for perturbation (as this quantity will cancel for all undisturbed PI chains. As such, if no target is specified,
// something has gone wrong.	
	
	const int P = (int)systems.size();  // Trotter number: number of beads (or "systems") in PI representation
	

	// To compute the weighted "orientational distance" of the "polymer chain", we must compute the difference vector between adjacent
	// bonds in our diatomic molecule. Systems 1 -> P should all have the same number and type of molecular constiutents. Molecule 1 in
	// System 1 is a partial representation of Molecule 1. Molecule 1 in System 2 is another part of that representation and each system
	// can be thought of as a "many worlds" representation where each member exists in every system but is affected by slightly different
	// circumstances. The complete representation is an average over all the "Molecule 1"'s in every system. But the "orientational 
	// distance" is a harmonic potential of sorts between the bond-vectors of each of these images. It makes the images form a coherent
	// representation of a single particle and prevents the diatomic beads from orienting themselves independently of each other. 

	double        PI_orient_diff = 0.0;     // "orientational difference", for lack of a better word. It is the (length^2 + length^2 + ...)
											// for each of P vectors describing the *difference* between bonds in adjacent images of a PI
	                                        // representation of a diatomic molecule in the system. It plays the role of energy if this were a 
	                                        // simulation of a harmonic potential. 

	
	std::vector<Vector3D> bond_vectors;
	char *moleculeID = systems[rank]->checkpoint->molecule_altered->moleculetype;
	int    orientation_site = SimulationControl::get_orientation_site( moleculeID );
	double bond_length      = SimulationControl::get_bond_length(      moleculeID );
	if (  (orientation_site < 0)   ||   (bond_length <= 0)  )
		return 0.0;

	// form the difference vectors 
	for_each( systems.begin(), systems.end(), [orientation_site, bond_length, & bond_vectors](System * s) {
		
		// Get COM for perturbation target
		Molecule *molecule = s->checkpoint->molecule_altered;
		molecule->update_COM();
		Vector3D rCOM( molecule->com[0], molecule->com[1], molecule->com[2] );

		// Create a vector locating the atomic site that is the designated "handle" for orienting the molecule (i.e. the orientation site)
		int site = 0;
		Atom *aPtr = nullptr;
		for (aPtr = molecule->atoms; site != orientation_site; aPtr = aPtr->next)
			site++;
		Vector3D handle_position( aPtr->pos[0], aPtr->pos[1], aPtr->pos[2]);

		// find the difference between the handle and the COM (the bond direction), and scale it such that the length of this vector
		// equals the bond length of the molecule (thereby representing the bond itself)
		Vector3D bond = handle_position - rCOM;
		bond = bond_length * bond.normalize();
		bond_vectors.push_back( bond );
	});
	
	// Cycle once through the bond vectors and sum the square-length of the difference vector between each adjacent pair, 
	// i.e. we will be finding dist^2 values for each pair of coordinates (x[i],y[i],z[i]) & (x[i+1], y[i+1], z[i+1]), 
	// such that the last difference will be between the last bond vector and the first.
	for (int i = 0; i < P; i++) {
		int j = (i + 1) % P;
		Vector3D diff_vector = bond_vectors[i] - bond_vectors[j];
		PI_orient_diff += diff_vector.norm2();
	}
	PI_orient_diff *= (ANGSTROM2METER * ANGSTROM2METER); // convert A^2 to m^2

	return PI_orient_diff;
}




int SimulationControl::PI_pick_NVT_move() {
// PI_pick_NVT_move() determines what move will be made next time make_move() is called and selects
// the molecule to which said move will be applied (and creates pointers to its list location).
// Returns the selected move

	int perturb_target = 0; // array index of molecule to be perturbed by next MC move,
	                        // in the array we are about to create.

	std::vector<Molecule *> perturbableMolecules;  // vector of perturbation-eligible molecules
	Molecule  * molecule_ptr       = nullptr;      // linked list traverser
	Molecule  * prev_molecule_ptr  = nullptr;      // molecule linked prev to "current" (in list)
	
	double spinflip_prob        = sys.spinflip_probability;
	double bead_perturb_prob    = sys.bead_perturb_probability;
	double dice_roll_for_move   = Rando::rand();
	double dice_roll_for_target = Rando::rand();


	for( int s=0; s < PI_nBeads; s++ ) {

		// populate array with pointers to elements in the linked list		
		for (molecule_ptr = systems[s]->molecules; molecule_ptr; molecule_ptr = molecule_ptr->next) {
			if (!(molecule_ptr->frozen || molecule_ptr->adiabatic || molecule_ptr->target))
				perturbableMolecules.push_back(molecule_ptr);
		}

		// select the target (i.e. pick one of the vector elements) & update checkpoint accordingly
		if (perturbableMolecules.size() == 0)
			throw no_molecules_in_system;
		perturb_target = (int)floor(perturbableMolecules.size() * dice_roll_for_target);
		systems[s]->checkpoint->molecule_altered = perturbableMolecules[perturb_target];


		// pick the move
		if (systems[s]->quantum_rotation && (dice_roll_for_move < spinflip_prob)) {
			systems[s]->checkpoint->movetype = MOVETYPE_SPINFLIP;
		}
		else if (dice_roll_for_move < (bead_perturb_prob + spinflip_prob)) {
			systems[s]->checkpoint->movetype = MOVETYPE_PERTURB_BEADS;
		}
		else {
			systems[s]->checkpoint->movetype = MOVETYPE_DISPLACE;
		}

		// Determine the head and tail of the selected molecule, checkpoint->head will be nullptr if molecule is 1st in list
		prev_molecule_ptr = nullptr;
		for (molecule_ptr = systems[s]->molecules; molecule_ptr; molecule_ptr = molecule_ptr->next) {
			if (molecule_ptr == systems[s]->checkpoint->molecule_altered) {
				systems[s]->checkpoint->head = prev_molecule_ptr;
				systems[s]->checkpoint->tail = molecule_ptr->next;
				break;
			}
			prev_molecule_ptr = molecule_ptr;
		}


		// if we have a molecule already backed up (from a previous accept), go ahead and free it
		if (systems[s]->checkpoint->molecule_backup) {
			// clear the references to the pair list, so that they aren't deleted in the process 
			systems[s]->checkpoint->molecule_backup->wipe_pair_refs();
			delete systems[s]->checkpoint->molecule_backup;
			systems[s]->checkpoint->molecule_backup = nullptr;
		}
		// backup the state that will be altered
		systems[s]->checkpoint->molecule_backup = new Molecule(*systems[s]->checkpoint->molecule_altered);

		perturbableMolecules.clear();

	}

	return systems[rank]->checkpoint->movetype;
}




void SimulationControl::PI_make_move(int movetype) {
// apply the move that was selected in the checkpoint

	// update the cavity grid prior to making a move 
	if (sys.cavity_bias)
		std::for_each(systems.begin(), systems.end(), [](System *SYS) {
			SYS->cavity_update_grid();
			SYS->checkpoint->biased_move = 0;
		});


	switch (movetype) {

		case MOVETYPE_DISPLACE:
			PI_displace();
			break;

		case MOVETYPE_SPINFLIP:
			PI_flip_spin();
			break;

		case MOVETYPE_PERTURB_BEADS:
			PI_perturb_beads();
			break;
		/*
			case MOVETYPE_ADIABATIC:
				// change coords of 'altered'
				displace(checkpoint->molecule_altered, pbc, adiabatic_probability, 1.0);
				break;

			case MOVETYPE_VOLUME:
				volume_change(); // I don't want to contribute to the god damned mess -- kmclaugh
				break;
		*/
		default:
			Output::err("MC_MOVES: invalid mc move\n");
			throw invalid_monte_carlo_move;
	}
}




/*
void SimulationControl::PI_insert() {

// int cavities_array_counter = 0;
// System::cavity_t  * cavities_array = nullptr;
// int random_index = 0;
// double com [3] = { 0 };
// double rand[3] = { 0 };
// Molecule  * molecule_ptr = nullptr;
// Atom      * atom_ptr = nullptr;
// Pair      * pair_ptr = nullptr;

// insert a molecule at a random pos and orientation
			// umbrella sampling
			if (cavity_bias && cavities_open) {
				// doing a biased move - this flag lets mc.c know about it
				checkpoint->biased_move = 1;
				// make an array of possible insertion points
				SafeOps::calloc(cavities_array, cavities_open, sizeof(cavity_t), __LINE__, __FILE__);
				for (int i = 0; i < cavity_grid_size; i++) {
					for (int j = 0; j < cavity_grid_size; j++) {
						for (int k = 0; k < cavity_grid_size; k++) {
							if (!cavity_grid[i][j][k].occupancy) {
								for (int p = 0; p < 3; p++)
									cavities_array[cavities_array_counter].pos[p] = cavity_grid[i][j][k].pos[p];
								++cavities_array_counter;
							}
						} // end k
					} // end j
				} // end i
				// insert randomly at one of the free cavity points
				random_index = (cavities_open - 1) - (int)rint(((double)(cavities_open - 1))*get_rand());
				for (int p = 0; p < 3; p++)
					com[p] = cavities_array[random_index].pos[p];
				// free the insertion array
				free(cavities_array);
			} // end umbrella

			else {
				// insert the molecule to a random location within the unit cell
				for (int p = 0; p < 3; p++)
					rand[p] = 0.5 - get_rand();
				for (int p = 0; p < 3; p++) {
					com[p] = 0;
					for (int q = 0; q < 3; q++)
						com[p] += pbc.basis[q][p] * rand[q];
				}
			}

			// process the inserted molecule
			for (atom_ptr = checkpoint->molecule_backup->atoms; atom_ptr; atom_ptr = atom_ptr->next) {
				// move the molecule back to the origin and then assign it to com
				for (int p = 0; p < 3; p++)
					atom_ptr->pos[p] += com[p] - checkpoint->molecule_backup->com[p];
			}

			// update the molecular com
			for (int p = 0; p < 3; p++)
				checkpoint->molecule_backup->com[p] = com[p];
			// give it a random orientation
			checkpoint->molecule_backup->rotate_rand_pbc(1.0); // , pbc, &mt_rand );

			// insert into the list
			if (num_insertion_molecules) {
				// If inserting a molecule from an insertion list, we will always insert at the end
				checkpoint->head->next = checkpoint->molecule_backup;
				checkpoint->molecule_backup->next = nullptr;
			}
			else {
				if (!checkpoint->head) {      // if we're at the start of the list:
					molecules = checkpoint->molecule_backup;
				}
				else {
					checkpoint->head->next = checkpoint->molecule_backup;
				}
				checkpoint->molecule_backup->next = checkpoint->molecule_altered;
			}

			// set new altered and tail to reflect the insertion
			checkpoint->molecule_altered = checkpoint->molecule_backup;
			checkpoint->tail = checkpoint->molecule_altered->next;
			checkpoint->molecule_backup = nullptr;

			if (num_insertion_molecules) { //multi sorbate
				// Free all pair memory in the list
				for (molecule_ptr = molecules; molecule_ptr; molecule_ptr = molecule_ptr->next) {
					for (atom_ptr = molecule_ptr->atoms; atom_ptr; atom_ptr = atom_ptr->next) {
						pair_ptr = atom_ptr->pairs;
						while (pair_ptr) {
							Pair *temp = pair_ptr;
							pair_ptr = pair_ptr->next;
							free(temp);
						}
					}
				}
				// Generate new pairs lists for all atoms in system
				allocate_pair_lists();
			} // only one sorbate
			else
				update_pairs_insert();

			//reset atom and molecule id's
			enumerate_particles();

			break;
*/
/*
void SimulationControl::PI_remove() {
	case MOVETYPE_REMOVE:

		// remove a randomly chosen molecule
		if (cavity_bias) {
			if (get_rand() < pow((1.0 - avg_observables->cavity_bias_probability), ((double)cavity_grid_size*cavity_grid_size*cavity_grid_size)))
				checkpoint->biased_move = 0;
			else
				checkpoint->biased_move = 1;
		}

		// remove 'altered' from the list
		if (!checkpoint->head) {	// handle the case where we're removing from the start of the list
			checkpoint->molecule_altered = molecules;
			molecules = molecules->next;
		}
		else {
			checkpoint->head->next = checkpoint->tail;
		}
		//free_molecule( system, system->checkpoint->molecule_altered );
		delete checkpoint->molecule_altered;
		update_pairs_remove();

		//reset atom and molecule id's
		enumerate_particles();

		break;
	}
*/




void SimulationControl::PI_flip_spin() {

	const int nSystems = (int)systems.size();

	for( int s=0; s<nSystems; s++ )
		if( systems[s]->checkpoint->molecule_altered->nuclear_spin == NUCLEAR_SPIN_PARA)
			systems[s]->checkpoint->molecule_altered->nuclear_spin = NUCLEAR_SPIN_ORTHO;
		else
			systems[s]->checkpoint->molecule_altered->nuclear_spin = NUCLEAR_SPIN_PARA;

	return;
}




void SimulationControl::PI_displace() {

	double dice_rolls[6];
	for (int i = 0; i < 6; i++)
		dice_rolls[i] = Rando::rand();
		
	int nSystems = (int) systems.size();
	
	if (sys.rd_anharmonic)
		// displace_1D(checkpoint->molecule_altered, sys.move_factor);
		throw unsupported_setting;

	if (sys.spectre)
		// spectre_displace(checkpoint->molecule_altered, move_factor, spectre_max_charge, spectre_max_target);
		throw unsupported_setting;

	if (sys.gwp) {
		// if (checkpoint->molecule_altered->atoms->gwp_spin) {
		//	   displace(checkpoint->molecule_altered, pbc, gwp_probability, rot_factor);
		//     checkpoint->molecule_altered->displace_gwp(gwp_probability, &mt_rand);
		// }
		// else
		//     displace(checkpoint->molecule_altered, pbc, move_factor, rot_factor);
		throw unsupported_setting; // remove once supported
	}

	Vector3D pi_com (0, 0, 0); // Center-of-Mass of the Path Integral representation of the molecule
	std::vector<Molecule *> altered_molecules;
	for (int s = 0; s < nSystems; s++) {
		// The "random translation/rotation" that is applied needs to be the same for each bead
		altered_molecules.push_back( systems[s]->checkpoint->molecule_altered );
		altered_molecules[s]->update_COM();
		altered_molecules[s]->translate_rand_pbc( sys.move_factor, systems[s]->pbc, dice_rolls );
		Vector3D molecule_com(altered_molecules[s]->com[0], altered_molecules[s]->com[1], altered_molecules[s]->com[2]);
		pi_com += molecule_com;
	}
	pi_com /= nSystems;

	// Create a random rotation for the molecule
	double diceX = Rando::rand_normal();
	double diceY = Rando::rand_normal();
	double diceZ = Rando::rand_normal();
	double dice_angle = Rando::rand() * sys.rot_factor;
	Quaternion rotation(diceX, diceY, diceZ, dice_angle, Quaternion::AXIS_ANGLE_DEGREE);

	// go to each molecule
	std::for_each( altered_molecules.begin(), altered_molecules.end(), [pi_com, rotation](Molecule *altered) {
		// move the molecule by -pi_com (positioning it as if the PI bead chain's center of mass was at the origin)
		altered->translate( -pi_com );
		
		// rotate each site about the random axis
		Atom *aPtr = altered->atoms;
		while (aPtr) {
			Vector3D atomic_pos( aPtr->pos[0], aPtr->pos[1], aPtr->pos[2] );
			atomic_pos = rotation.rotate(atomic_pos);
			aPtr->pos[0] = atomic_pos.x();
			aPtr->pos[1] = atomic_pos.y();
			aPtr->pos[2] = atomic_pos.z();
			aPtr = aPtr->next;
		}
		
		// move the molecule by  pi_com
		altered->translate( pi_com );
		altered->update_COM();
	});

}




void SimulationControl::PI_perturb_beads() {
	PI_perturb_beads_orientations();
	PI_perturb_bead_COMs();
}




void SimulationControl::PI_perturb_bead_COMs_ENTIRE_SYSTEM(double scale) {

	Molecule *molPtr = nullptr;
	int nSystems = (int)systems.size();
	std::vector<Molecule *> molecules_ptr;   // this array of ptrs tracks the current molecule in each system 
	std::vector<Molecule *> altered_backup;   
	molecules_ptr.resize(nSystems);
	altered_backup.resize(nSystems);

	
	// Backup the perturbation scale factor that was set by the user...
	double scale_bak = sys.PI_bead_perturb_factor;
	sys.PI_bead_perturb_factor = scale;
	// ...as well as the molecules targeted for perturbation (if any)
	for (int s = 0; s < nSystems; s++) {
		molecules_ptr [s] = systems[s]->molecules; 
		altered_backup[s] = systems[s]->checkpoint->molecule_altered;
	}

	// Step through each perturb-able molecule in lockstep (each image of that molecule in all systems). At each step, the
	// current set of molecules are set as the perturb targets for the group of systems (checkpoint->molecule_altered, for
	// each sys) and then the set is perturbed and we advance to the next perturbable molecule in each system.
	while( molPtr = molecules_ptr[0] ) {
	
		// Check to see if the current molecule should be perturbed
		// Corresponding molecules *should* be the same across all systems, and the entries of molecules_ptr should point to 
		// different images of the same molecule, so only the version of the molecule in system 0 (molPtr) is checked for validity
		if ( ! (molPtr->frozen || molPtr->adiabatic || molPtr->target)) {

			// If the molecule in image/sys 0 looked good, we target this molecule for perturbation in all system images...
			for (int s = 0; s < nSystems; s++) {
				if (nullptr == molecules_ptr[s]) {
					// ... assuming corresponding molecules exist, that is.
					Output::err("ERROR: System images are not consistent, head system has more molecules than at least one other system image.\n");
					throw internal_error;
				}
				systems[s]->checkpoint->molecule_altered = molecules_ptr[s];
			}
			// Perturb all the beads for the targeted molecule in every image
			PI_perturb_bead_COMs(nSystems);
		}
		
		// advance our collection of molecule pointers to the next molecule in their respective lists
		for( int s=0; s<nSystems; s++ )
			molecules_ptr[s] = molecules_ptr[s]->next;
	}

	// Restore the targeted molecules and the bead perturbation scaling factor to their original states
	sys.PI_bead_perturb_factor = scale_bak;
	for (int s = 0; s < nSystems; s++) 
		systems[s]->checkpoint->molecule_altered = altered_backup[s];


}
void SimulationControl::PI_perturb_bead_COMs() {
	PI_perturb_bead_COMs( PI_trial_chain_length ); // number of beads in a "trial chain" -- the number beads to move when generating trial COM configs)
}
void SimulationControl::PI_perturb_bead_COMs(int n) {
// The algorithm for center-of-mass bead perturbation methods comes from:
// Coker et al.;  J.Chem.Phys. 86, 5689 (1987); doi: 10.1063/1.452495
// n is the number of beads to move

	double beta = 1.0/(kB*sys.temperature);

	static int starterBead = 0; // This bead will not move, and is the index of the first bead to act as an "initial bead". This function will
	                            // construct shorter and shorter chains of "trial chains" with an anchor bead at the start of each chain, this
	                            // this  is the index of the first bead of the first chain. In lang of the Coker ref, starterBead is r[1] in
	                            // the density matrix K(r[1], r[p+1]; beta). We advance starterBead each time we hit this fxn so that the 
	                            // "anchor beads" are different each time, and the effected area of our perturbations has a tendency to rotate
	                            // around the PI loop. The indices of all other participating beads are computed relative to starterBead.

	
	int prevBead_idx  = starterBead;                   // index of the bead that comes directly before the bead that is currently being moved 
	                                                   // in the PI chain (this bead will remain stationary).
	int bead_idx      = (prevBead_idx+1)   % PI_nBeads; // index of the bead that is currently being moved
	int finalBead_idx = (prevBead_idx+n+1) % PI_nBeads; // final bead of "trial chain"  (this bead is also stationary, and may be the same as prevBead)
	starterBead       = (starterBead+1)    % PI_nBeads; // advance the index for the starter bead for the next time this function is called
	
	double Mass = AMU2KG * systems[0]->checkpoint->molecule_altered->mass; // mass of the molecule whose bead configuration is being perturbed
	

	
	
	// populate a vector of Vecs with the COM data from the selected molecule, and compute the center-of-mass (COM) for the PI bead chain
	std::vector<Vector3D> beads;
	Vector3D chain_COM(0, 0, 0);
	for (int s = 0; s < PI_nBeads; s++) {
		
		systems[s]->checkpoint->molecule_altered->update_COM();

		double x = systems[s]->checkpoint->molecule_altered->com[0];
		double y = systems[s]->checkpoint->molecule_altered->com[1];
		double z = systems[s]->checkpoint->molecule_altered->com[2];
		Vector3D bead_COM(x, y, z);
		beads.push_back( bead_COM );
		chain_COM += bead_COM;
	}
	chain_COM /= PI_nBeads;

	// compute the perturbation for bead COM positions

	double tB = n;       // this corresponds to all non-cancelling factors of t[ i ] in reference
	double tA = n + 1;   // this corresponds to t[i-1] in the same
	                     // the reference has more factors, but they all cancel such that tB/tA is all that remains.

	for( int j=1; j<=n; j++ ) 
	{
		double init_factor  = tB-- / tA--;          // t[i] / t[i-1]    Eqs. 3.9 & 3.10  (seq is e.g. 4/5 -> 3/4 -> 2/3 -> 1/2)
		double term_factor  = 1.0 - init_factor;    // tau  / t[i-1]    Eq.  3.9
		double sigma_factor = sys.PI_bead_perturb_factor * sqrt(  (hBar2*beta*init_factor) / (PI_nBeads*Mass)  ) * METER2ANGSTROM;  // Eqs. 3.10/3.12 + conv m -> Angstroms
		
		// create the Vec(tor) along which the target bead will be perturbed out of its 'average' position
		Vector3D perturbation( Rando::rand_normal(), Rando::rand_normal(), Rando::rand_normal() ); 

		//                  |------ weighted average pos on line connecting existing beads ------|     |------ perturbation ------|
		beads[bead_idx]  =  (init_factor*beads[prevBead_idx]) + (term_factor*beads[finalBead_idx])  +  (sigma_factor*perturbation); // Eq. 3.12 
		

		prevBead_idx = (prevBead_idx + 1) % PI_nBeads;  // advance prevBead index
		bead_idx     = (prevBead_idx + 1) % PI_nBeads;  // current bead index will be 1 greater than prevBead
	}

	// Compute the center of mass for the chain, post-perturbation
	Vector3D delta_COM(0, 0, 0);
	for_each(beads.begin(), beads.end(), [&delta_COM](Vector3D bead) {
		delta_COM += bead;
	});
	delta_COM /= PI_nBeads;
	delta_COM = delta_COM - chain_COM;

	// Shift the COM coord of the individual beads, such that the COM of the entire chain leaves this method unchanged 
	for_each(beads.begin(), beads.end(), [delta_COM](Vector3D &bead) {
		bead -= delta_COM;
	});

	// now impose the perturbations we've computed back onto the actual system representations
	for (int s = 0; s < PI_nBeads; s++)
		systems[s]->checkpoint->molecule_altered->move_to_(beads[s].x(), beads[s].y(), beads[s].z());
}




void SimulationControl::PI_perturb_beads_orientations() {
	char * moleculeID       = systems[0]->checkpoint->molecule_altered->moleculetype;
	int    orientation_site = SimulationControl::get_orientation_site(moleculeID);
	double bond_length      = SimulationControl::get_bond_length(moleculeID);

	// Exit early if requisite orientation data is not present
	if (  (orientation_site < 0)  ||  (bond_length <= 0)  )
		return;
	
	generate_orientation_configs();
	apply_orientation_configs();
}




void SimulationControl::generate_orientation_configs() {
	
	double sorbate_reduced_mass = SimulationControl::get_reduced_mass(systems[0]->checkpoint->molecule_altered->moleculetype);
	if (sorbate_reduced_mass < 0) {
		char buffer[maxLine];
		sprintf(buffer, "No reduced mass specified for moveable/sorbate molecule \"%s\"\n", systems[0]->checkpoint->molecule_altered->moleculetype);
		Output::err(buffer);
		throw missing_required_datum;
	}
	
	double sorbate_bond_length = SimulationControl::get_bond_length(systems[0]->checkpoint->molecule_altered->moleculetype);
	if (sorbate_bond_length < 0) {
		char buffer[maxLine];
		sprintf(buffer, "No bond length specified for moveable/sorbate molecule \"%s\"\n", systems[0]->checkpoint->molecule_altered->moleculetype);
		Output::err(buffer);
		throw missing_required_datum;
	}
	sorbate_bond_length /= METER2ANGSTROM;
	double b2 = sorbate_bond_length * sorbate_bond_length;

	double u_kB_T = sorbate_reduced_mass * kB * sys.temperature;
	orientations[0].randomize();
	generate_orientation_configs(0, (unsigned int)orientations.size(), 2, (unsigned int)orientations.size(), b2, u_kB_T );
}
void SimulationControl::generate_orientation_configs(unsigned int start, unsigned int end, unsigned int p, unsigned int numBeads, double b2, double ukT ) {
// This algorithm comes from:
// Subramanian et al.;  J. Chem. Phys. 146, 094105 (2017); doi: 10.1063/1.4977597]

	const double two_PI = 2.0 * pi;
	

	if (p <= numBeads) {

		// We are given two vectors, I and K, and we want to place J.   J will be halfway between I and K on the polymer/bead
		// chain. We start with a "spring constant" that varies in a wide range, but is consistent with the orientation range 
		// of the entire quantum particle. At the end of the process the spring constant used for placement exactly describes
		// the distribution, and I & K will be directly adjacent beads (to J) on the bead chain. 
		unsigned int J_idx = (start + end) / 2;
		unsigned int K_idx = (end == numBeads) ? 0 : end;
		Vector3D vec_I = orientations[start];
		Vector3D vec_K = orientations[K_idx];



		// "bisector" is the vector that is the exact middle position between the input vectors, I & K. It will be the starting
		// point for the perturbation---normalized since we intend to use it as an axis of rotation.
		Vector3D bisector((vec_I + vec_K) / 2.0);
		bisector.normalize();


		// vec_IK is a vector that is orthogonal to bisector. In most cases, the vector extending from I to K will be 
		// orthogonal already. However, in the initial case, I and K are the same vector, so we create a vector that is
		// only special in the fact that it is different from bisector. We take a cross product with this vector to find  
		// a vector that is orthogonal to bisector.
		Vector3D vec_IK;
		double psi_IK = 0; // this is the angle (in radians) between the vectors I & K

		if (p > 2) {
			vec_IK = vec_K - vec_I;
			psi_IK = Vector3D::angle(vec_I, vec_K);
		}
		else {
			// In this case, the angle between I & K is 0, so we leave psi_IK as-is.
			vec_IK.set(1, 2, -3);                       // here vec_IK is just acting as a temp/dummy variable
			Vector3D different_vec = vec_IK + bisector; // different_vec is simply a different vector than bisector
			different_vec.normalize();
			vec_IK = different_vec.cross(bisector);     // (different_vec X bisector) is ortho to bisector
		}


		double C = Rando::rand(); 
		double lambda2 = h*h / (two_PI*ukT);           // lambda^2 for reduced mass of molecule (thermal wavelength^2) 
		double kh = pi * b2 / lambda2;                 // Eq (13b) in ref
		double K = 4.0 * kh * p * cos( psi_IK * 0.5 ); // Eq (17b) in ref

		// Alpha in reference (renamed to be consistent with angle_B)
		const double angle_A = acos(1.0 + (1.0/K) * log( 1.0 - C*(1.0 - exp(-2.0*K)))); // Eq. (18) in ref
		const double angle_B = Rando::rand() * two_PI;  // Beta in reference  (renamed to avoid confusion with 1/kT)


		// We rotate our ortho vector, IK, about bisector by Beta, and this will form the axis about which bisector will
		// be rotated by Alpha in order to arrive at our final orientation vector. The plane through which bisector travels
		// is defined by the vectors bisector and (bisector X vec_Beta), that is, a vector that would define the choice of 
		// Beta (per the reference) is ortho to both bisector and IK. Since vec_Beta was chosen at random from [0..2pi] about 
		// bisector, this means (bisector X vec_Beta) is likewise randomly chosen from [0..2pi] about bisector.
		Quaternion betaRotation(bisector.x(), bisector.y(), bisector.z(), angle_B, Quaternion::AXIS_ANGLE_RADIAN);
		Vector3D vec_Beta = betaRotation.rotate(vec_IK);
		
		// Construct the final rotation axis. The bisector vector (the exact average orientation vector), will be rotated in the 
		// direction of the beta vector by an angle alpha (angle_A), to arrive at the unit vector that will define the orientation
		// of the bead we are placing. 
		Quaternion final_rotation(vec_Beta.x(), vec_Beta.y(), vec_Beta.z(), angle_A, Quaternion::AXIS_ANGLE_RADIAN);
		
		// Finally, move the bisector vector--the average orientation given I and K--toward beta by an angle alpha to realize 
		// create the perturbation. It is already a unit vector, so we record the orientation for use later.
		Vector3D vec_J = final_rotation.rotate(bisector);
		orientations[J_idx].set(vec_J.x(), vec_J.y(), vec_J.z());
		
		// Now that the intermediate orientation has been perturbed and set, we repeat the procedure by placing two more
		// perturbed, intermediate, orientation vectors between I and J, and between J and K
		if (p < numBeads) {
			generate_orientation_configs( start, J_idx, p * 2, numBeads, b2, ukT);
			generate_orientation_configs( J_idx,   end, p * 2, numBeads, b2, ukT);
		}
	}
}




void SimulationControl::apply_orientation_configs() {
// Orient the beads representing a molecule according to a set of vectors that specify each bead's orientation in space
// Traverse the beads for a given molecule and orient each molecule according to its respective orientation vector.

	int nSystems = (int) systems.size();

	// Impose the orientational perturbations we've computed onto the actual system representations
	int orientation_site = SimulationControl::get_orientation_site( systems[0]->checkpoint->molecule_altered->moleculetype );
	if (orientation_site < 0)
		return; // Molecule has no "handle" specified, and so is will not be re-oriented. 
	for (int s = 0; s < nSystems; s++) {
		systems[s]->checkpoint->molecule_altered->orient( orientations[s], orientation_site );
	}
}