/* +---------------------------------------------------------------------------+
   |          The Mobile Robot Programming Toolkit (MRPT) C++ library          |
   |                                                                           |
   |                       http://www.mrpt.org/                                |
   |                                                                           |
   |   Copyright (C) 2005-2011  University of Malaga                           |
   |                                                                           |
   |    This software was written by the Machine Perception and Intelligent    |
   |      Robotics Lab, University of Malaga (Spain).                          |
   |    Contact: Jose-Luis Blanco  <jlblanco@ctima.uma.es>                     |
   |                                                                           |
   |  This file is part of the MRPT project.                                   |
   |                                                                           |
   |     MRPT is free software: you can redistribute it and/or modify          |
   |     it under the terms of the GNU General Public License as published by  |
   |     the Free Software Foundation, either version 3 of the License, or     |
   |     (at your option) any later version.                                   |
   |                                                                           |
   |   MRPT is distributed in the hope that it will be useful,                 |
   |     but WITHOUT ANY WARRANTY; without even the implied warranty of        |
   |     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         |
   |     GNU General Public License for more details.                          |
   |                                                                           |
   |     You should have received a copy of the GNU General Public License     |
   |     along with MRPT.  If not, see <http://www.gnu.org/licenses/>.         |
   |                                                                           |
   +---------------------------------------------------------------------------+ */

#include <mrpt/maps.h>  // Precompiled header



#include <mrpt/slam/CWirelessPowerGridMap2D.h>
#include <mrpt/slam/CObservationWirelessPower.h>
#include <mrpt/system/os.h>
#include <mrpt/math/utils.h>
#include <mrpt/utils/CTicTac.h>
#include <mrpt/utils/color_maps.h>

#include <mrpt/opengl.h>
#include <numeric>

using namespace mrpt;
using namespace mrpt::slam;
using namespace mrpt::utils;
using namespace mrpt::poses;
using namespace std;

IMPLEMENTS_SERIALIZABLE(CWirelessPowerGridMap2D, CMetricMap,mrpt::slam)

/*---------------------------------------------------------------
						Constructor
  ---------------------------------------------------------------*/
CWirelessPowerGridMap2D::CWirelessPowerGridMap2D(
	TMapRepresentation	mapType,
	float		x_min,
	float		x_max,
	float		y_min,
	float		y_max,
	float		resolution ) :
		CDynamicGrid<TWirelessPowerCell>( x_min,x_max,y_min,y_max,resolution ),
		insertionOptions(),
		m_debug_dump(NULL),
		decimate_count(1),
		fixed_incT(0),
		first_incT(true),
		m_mapType(mapType),
		m_cov(0,0),
		m_hasToRecoverMeanAndCov(true),
		m_DM_lastCutOff(0),
		m_average_normreadings_mean(0),
		m_average_normreadings_var(0),
		m_average_normreadings_count(0)
{
	// Set the grid to initial values (and adjusts the KF covariance matrix!)
	CMetricMap::clear();
}

CWirelessPowerGridMap2D::~CWirelessPowerGridMap2D()
{
	mrpt::utils::delete_safe(m_debug_dump);
}

/*---------------------------------------------------------------
						clear
  ---------------------------------------------------------------*/
void  CWirelessPowerGridMap2D::internal_clear()
{
	m_average_normreadings_mean  = 0;
	m_average_normreadings_var   = 0;
	m_average_normreadings_count = 0;

	switch (m_mapType)
	{
	case mrKernelDM:
	case mrKernelDMV:
		{
			// Set the grid to initial values:
			TWirelessPowerCell	def(0,0);
			fill( def );
		}
		break;

	case mrKalmanFilter:
		{
			printf("[CWirelessPowerGridMap2D::clear] Setting covariance matrix to %ux%u\n",(unsigned int)(m_size_y*m_size_x),(unsigned int)(m_size_y*m_size_x));

			TWirelessPowerCell	def(
				insertionOptions.KF_defaultCellMeanValue,		// mean
				insertionOptions.KF_initialCellStd				// std
				);

			fill( def );

			// Reset the covariance matrix:
			m_cov.setSize( m_size_y*m_size_x, m_size_y*m_size_x );

			// And load its default values:
			const double KF_covSigma2 = square(insertionOptions.KF_covSigma);
			const double res2 = square(m_resolution);
			const double std0sqr = square(  insertionOptions.KF_initialCellStd );

			for (size_t i = 0;i<m_cov.getRowCount();i++)
			{
				int		cx1 = ( i % m_size_x );
				int		cy1 = ( i / m_size_x );

				for (size_t j = i;j<m_cov.getColCount();j++)
				{
					int		cx2 = ( j % m_size_x );
					int		cy2 = ( j / m_size_x );

					if (i==j)
					{
						m_cov(i,j) = std0sqr;
					}
					else
					{
						m_cov(i,j) = std0sqr * exp( -0.5 * (res2 * static_cast<double>(square(cx1-cx2) +  square(cy1-cy2)))/KF_covSigma2 );
						m_cov(j,i) = m_cov(i,j);
					}
				} // for j
			} // for i

			//m_cov.saveToTextFile("cov_init.txt",1);
		}
		break;
		// and continue with:
	case mrKalmanApproximate:
		{

			m_hasToRecoverMeanAndCov = true;

			CTicTac	tictac;
			tictac.Tic();

			//printf("[CWirelessPowerGridMap2D::clear] Resetting compressed cov. matrix and cells\n");
			TWirelessPowerCell	def(
				insertionOptions.KF_defaultCellMeanValue,									// mean
				insertionOptions.KF_initialCellStd		// std
				);

			fill( def );

			// Reset the covariance matrix:
			// --------------------------------------
			const signed W = insertionOptions.KF_W_size;
			const size_t N = m_map.size();
			const size_t K = 2*W*(W+1)+1;

			const double KF_covSigma2 = square(insertionOptions.KF_covSigma);
			const double std0sqr = square( insertionOptions.KF_initialCellStd );
			const double res2 = square(m_resolution);

			m_stackedCov.setSize( N, K );

			// Populate it with the initial cov. values:
			// ------------------------------------------
			signed Acx, Acy;
			const double	*ptr_first_row = m_stackedCov.get_unsafe_row(0);

			for (size_t i=0;i<N;i++)
			{
				double	*ptr = m_stackedCov.get_unsafe_row(i);

				if (i==0)
				{
					// 1) current cell
					*ptr++ = std0sqr;

					// 2) W rest of the first row:
					Acy = 0;
					for (Acx=1;Acx<=W;Acx++)
						*ptr++ = std0sqr * exp( -0.5 * (res2 * static_cast<double>(square(Acx) +  square(Acy)))/KF_covSigma2 );

					// 3) The others W rows:
					for (Acy=1;Acy<=W;Acy++)
						for (Acx=-W;Acx<=W;Acx++)
							*ptr++ = std0sqr * exp( -0.5 * (res2 * static_cast<double>(square(Acx) +  square(Acy)))/KF_covSigma2 );
				}
				else
				{
					// Just copy the same:
					memcpy( ptr,ptr_first_row, sizeof(double)*K );
				}
			}

			//printf("[CWirelessPowerGridMap2D::clear] %ux%u cells done in %.03fms\n", unsigned(m_size_x),unsigned(m_size_y),1000*tictac.Tac());
		}
		break;
	};

}

/*---------------------------------------------------------------
						isEmpty
  ---------------------------------------------------------------*/
bool  CWirelessPowerGridMap2D::isEmpty() const
{
	return false;
}


/*---------------------------------------------------------------
						save_log_map
  ---------------------------------------------------------------*/
void CWirelessPowerGridMap2D::save_log_map(
	const mrpt::system::TTimeStamp	timestamp,
	const float						reading,
	const float						estimation,
	const float						k,
	const double					yaw,
	const float						speed
	)
{

	//function to save in a log file the information of the generated map

	double time = mrpt::system::timestampTotime_t(timestamp);
	char buffer [50];
	sprintf (buffer, "./log_MOSmodel_MAP_%X.txt",insertionOptions.sensorType);

	if (!m_debug_dump)
		m_debug_dump=new ofstream(buffer);

	if (m_debug_dump->is_open())
	{
		*m_debug_dump << format("%f \t", time );
		*m_debug_dump << format("%f \t", reading );
		*m_debug_dump << format("%f \t", estimation );
		*m_debug_dump << format("%f \t", k );
		*m_debug_dump << format("%f \t", yaw );
		*m_debug_dump << format("%f \t", speed );
		*m_debug_dump << "\n";
	}
	else cout << "Unable to open file";
}

/*---------------------------------------------------------------
						insertObservation
  ---------------------------------------------------------------*/
bool  CWirelessPowerGridMap2D::internal_insertObservation(
	const CObservation	*obs,
	const CPose3D			*robotPose )
{
	MRPT_START;

	CPose2D		robotPose2D;
	CPose3D		robotPose3D;

	if (robotPose)
	{
		robotPose2D = CPose2D(*robotPose);
		robotPose3D = (*robotPose);
	}
	else
	{
		// Default values are (0,0,0)
	}

	if ( IS_CLASS(obs, CObservationWirelessPower ))
	{
		/********************************************************************
					OBSERVATION TYPE: CObservationWirelessPower
		********************************************************************/
		const CObservationWirelessPower	*o = static_cast<const CObservationWirelessPower*>( obs );
		float						sensorReading;

		//Cover all robot e-noses (m_readings)

		// Get index to differentiate between enoses --> insertionoptions.enose_id
//		for (std::vector<CObservationWirelessPower::TObservationENose>::const_iterator it = o->m_readings.begin(); it!=o->m_readings.end();it+=1)

//		ASSERT_(o->m_readings.size() > insertionOptions.enose_id);
//		const CObservationWirelessPower::TObservationENose *it = &o->m_readings[insertionOptions.enose_id];
//		{
			// Compute the 3D sensor pose in world coordinates:
		/// TEMPORARY SOLUTION
			CPose2D		sensorPose( CPose3D(robotPose2D) + CPose3D(0,0,0) );

			// Compute the sensor reading value (Volts):
		/*	if (insertionOptions.sensorType==0x0000){	//compute the mean
				sensorReading = math::mean( it->readingsVoltage );
			}
			else
			{
				// Look for the correct sensor type
				size_t i;
				for (i=0; i<it->sensorTypes.size(); i++)
				{
					if (it->sensorTypes.at(i) == int(insertionOptions.sensorType) )
						break;
				}

				if (i<it->sensorTypes.size()){
					sensorReading = it->readingsVoltage[i];
				}
				else
				{
					//Sensor especified not found, compute default mean value
					sensorReading = math::mean( it->readingsVoltage );
				}
			}*/
			sensorReading = o->power;

			// Conversion Voltage(V)-->1/Resistance(1/Ohms)
			//sensorReading = 1/ (5 * insertionOptions.VoltageDivider_Res /sensorReading - insertionOptions.VoltageDivider_Res);

			// Normalization:
			sensorReading = (sensorReading - insertionOptions.R_min) /( insertionOptions.R_max - insertionOptions.R_min );


			// MOS model
		//	if(insertionOptions.useMOSmodel)
		//	{
				// Anti-Noise filtering
				noise_filtering(sensorReading, sensorPose, o->timestamp);

				//Decimate
				if ( decimate_count != insertionOptions.decimate_value ){
					decimate_count++;
					return true;
				}

				//wifi concentration estimation based on FIRST ORDER + NONLINEAR COMPENSATIONS DYNAMICS
				CWirelessPower_estimation(m_antiNoise_window[insertionOptions.winNoise_size/2].reading_filtered, m_antiNoise_window[insertionOptions.winNoise_size/2].sensorPose, m_antiNoise_window[insertionOptions.winNoise_size/2].timestamp);
				decimate_count = 1;

				//Use estimation to generate the map
				std::vector<TdataMap>::iterator iter = m_lastObservations.begin();
				sensorReading = iter->estimation;
				sensorPose = CPose2D(iter->sensorPose);

				//Save data map in log file for Matlab visualization
				if (insertionOptions.save_maplog)
					save_log_map(iter->timestamp, iter->reading, iter->estimation, iter->k, iter->sensorPose.yaw(), iter->speed);
		//	}

			// Update the gross estimates of mean/vars for the whole reading history (see IROS2009 paper):
			m_average_normreadings_mean = (sensorReading + m_average_normreadings_count*m_average_normreadings_mean)/(1+m_average_normreadings_count);
			m_average_normreadings_var  = (square(sensorReading - m_average_normreadings_mean) + m_average_normreadings_count*m_average_normreadings_var) /(1+m_average_normreadings_count);
			m_average_normreadings_count++;

#if 0
			cout << "[DEBUG] m_average_normreadings_count: " << m_average_normreadings_count << " -> mean: " << m_average_normreadings_mean << " var: " <<  m_average_normreadings_var  << endl;
#endif

			// Finally, do the actual map update with that value:
			switch (m_mapType)
			{
				case mrKernelDM:           insertObservation_KernelDM_DMV(sensorReading,sensorPose, false); break;
				case mrKernelDMV:          insertObservation_KernelDM_DMV(sensorReading,sensorPose, true); break;
				case mrKalmanFilter:       insertObservation_KF(sensorReading,sensorPose); break;
				case mrKalmanApproximate:  insertObservation_KF2(sensorReading,sensorPose);
			};

	//	} // for each e-nose obs.

		return true;	// Done!

	} // end if "CObservationWirelessPower"

	return false;

	MRPT_END;
}

/*---------------------------------------------------------------
					insertObservation_KernelDM_DMV
  ---------------------------------------------------------------*/
/** The implementation of "insertObservation" for Achim Lilienthal's map models DM & DM+V.
* \param normReading Is a [0,1] normalized concentration reading.
* \param sensorPose Is the sensor pose on the robot
* \param is_DMV = false -> map type is Kernel DM; true -> map type is DM+V
*/
void  CWirelessPowerGridMap2D::insertObservation_KernelDM_DMV(
	float			normReading,
	const CPose3D	&sensorPose_,
	bool             is_DMV )
{
	MRPT_START;

	static const TWirelessPowerCell defCell(0,0);

	const TPose3D sensorPose= TPose3D(sensorPose_);
	const TPose2D sensorPose2D = TPose2D( sensorPose );

	// Assure we have room enough in the grid!
	resize(	sensorPose2D.x - insertionOptions.cutoffRadius*2,
			sensorPose2D.x + insertionOptions.cutoffRadius*2,
			sensorPose2D.y - insertionOptions.cutoffRadius*2,
			sensorPose2D.y + insertionOptions.cutoffRadius*2,
			defCell );

	// Compute the "parzen Gaussian" once only:
	// -------------------------------------------------
	int						Ac_cutoff = round(insertionOptions.cutoffRadius / m_resolution);
	unsigned				Ac_all = 1+2*Ac_cutoff;
	double					minWinValueAtCutOff = exp(-square(insertionOptions.cutoffRadius/insertionOptions.sigma) );

	if ( m_DM_lastCutOff!=insertionOptions.cutoffRadius ||
			m_DM_gaussWindow.size() != square(Ac_all) )
	{
		printf("[CWirelessPowerGridMap2D::insertObservation_KernelDM_DMV] Precomputing window %ux%u\n",Ac_all,Ac_all);

		double	dist;
		double	std = insertionOptions.sigma;

		// Compute the window:
		m_DM_gaussWindow.resize(Ac_all*Ac_all);
		m_DM_lastCutOff=insertionOptions.cutoffRadius;

		// Actually the array could be 1/4 of this size, but this
		// way it's easier and it's late night now :-)
		vector<float>::iterator	it = m_DM_gaussWindow.begin();
        for (unsigned cx=0;cx<Ac_all;cx++)
		{
			for (unsigned cy=0;cy<Ac_all;cy++)
			{
				dist = m_resolution * sqrt( static_cast<double>(square( Ac_cutoff+1-cx ) + square( Ac_cutoff+1-cy ) ) );
				*(it++) = std::exp( - square(dist/std) );
			}
		}

		printf("[CWirelessPowerGridMap2D::insertObservation_KernelDM_DMV] Done!\n");
	} // end of computing the gauss. window.

	//	Fuse with current content of grid (the MEAN of each cell):
	// --------------------------------------------------------------
	const int sensor_cx = x2idx( sensorPose.x );
	const int sensor_cy = y2idx( sensorPose.y );
	TWirelessPowerCell	*cell;
	vector<float>::iterator	windowIt = m_DM_gaussWindow.begin();

	for (int Acx=-Ac_cutoff;Acx<=Ac_cutoff;Acx++)
	{
		for (int Acy=-Ac_cutoff;Acy<=Ac_cutoff;++Acy, ++windowIt)
		{
			const double windowValue = *windowIt;

			if (windowValue>minWinValueAtCutOff)
			{
				cell = cellByIndex(sensor_cx+Acx,sensor_cy+Acy);
				ASSERT_( cell!=NULL )

				cell->dm_mean_w  += windowValue;
				cell->dm_mean += windowValue * normReading;
				if (is_DMV)
				{
					const double cell_var = square(normReading - computeMeanCellValue_DM_DMV(cell) );
					cell->dmv_var_mean += windowValue * cell_var;
				}
			}
		}
	}

	MRPT_END;
}


/*---------------------------------------------------------------
						computeObservationLikelihood
  ---------------------------------------------------------------*/
double	 CWirelessPowerGridMap2D::computeObservationLikelihood(
	const CObservation		*obs,
	const CPose3D			&takenFrom )
{
	MRPT_UNUSED_PARAM(obs);MRPT_UNUSED_PARAM(takenFrom);

    THROW_EXCEPTION("Not implemented yet!");
}

/*---------------------------------------------------------------
  Implements the writing to a CStream capability of CSerializable objects
 ---------------------------------------------------------------*/
void  CWirelessPowerGridMap2D::writeToStream(CStream &out, int *version) const
{
	if (version)
		*version = 3;
	else
	{
		uint32_t	n;

		// Save the dimensions of the grid:
		out << m_x_min << m_x_max << m_y_min << m_y_max;
		out << m_resolution;
		out << static_cast<uint32_t>(m_size_x) << static_cast<uint32_t>(m_size_y);

		// To assure compatibility: The size of each cell:
		n = static_cast<uint32_t>(sizeof( TWirelessPowerCell ));
		out << n;

		// Save the map contents:
		n = static_cast<uint32_t>(m_map.size());
		out << n;

		// Save the "m_map": This requires special handling for big endian systems:
#if MRPT_IS_BIG_ENDIAN
		for (uint32_t i=0;i<n;i++)
		{
			out << m_map[i].kf_mean << m_map[i].dm_mean << m_map[i].dmv_var_mean;
		}
#else
		// Little endian: just write all at once:
		out.WriteBuffer( &m_map[0], sizeof(m_map[0])*m_map.size() );  // TODO: Do this endianness safe!!
#endif


		// Version 1: Save the insertion options:
		out << uint8_t(m_mapType)
			<< m_cov
			<< m_stackedCov;

		out << insertionOptions.sigma
			<< insertionOptions.cutoffRadius
			<< insertionOptions.R_min
			<< insertionOptions.R_max
			<< insertionOptions.KF_covSigma
			<< insertionOptions.KF_initialCellStd
			<< insertionOptions.KF_observationModelNoise
			<< insertionOptions.KF_defaultCellMeanValue
			<< insertionOptions.KF_W_size;

		// New in v3:
		out << m_average_normreadings_mean << m_average_normreadings_var << uint64_t(m_average_normreadings_count);

	}
}

// Aux struct used below (must be at global scope for STL):
struct TOldCellTypeInVersion1
{
	float	mean, std;
	float	w,wr;
};

/*---------------------------------------------------------------
  Implements the reading from a CStream capability of CSerializable objects
 ---------------------------------------------------------------*/
void  CWirelessPowerGridMap2D::readFromStream(CStream &in, int version)
{
	switch(version)
	{
	case 0:
	case 1:
	case 2:
	case 3:
		{
			uint32_t	n,i,j;

			// Load the dimensions of the grid:
			in >> m_x_min >> m_x_max >> m_y_min >> m_y_max;
			in >> m_resolution;
			in >> i >> j;
			m_size_x = i;
			m_size_y = j;

			// To assure compatibility: The size of each cell:
			in >> n;

			if (version<2)
			{	// Converter from old versions <=1
				ASSERT_( n == static_cast<uint32_t>( sizeof( TOldCellTypeInVersion1 ) ));
				// Load the map contents in an aux struct:
				in >> n;
				vector<TOldCellTypeInVersion1>  old_map(n);
				in.ReadBuffer( &old_map[0], sizeof(old_map[0])*old_map.size() );

				// Convert to newer format:
				m_map.resize(n);
				for (size_t k=0;k<n;k++)
				{
					m_map[k].kf_mean = (old_map[k].w!=0) ? old_map[k].wr : old_map[k].mean;
					m_map[k].kf_std  = (old_map[k].w!=0) ? old_map[k].w  : old_map[k].std;
				}
			}
			else
			{
				ASSERT_EQUAL_( n , static_cast<uint32_t>( sizeof( TWirelessPowerCell ) ));
				// Load the map contents:
				in >> n;
				m_map.resize(n);

				// Read the note in writeToStream()
#if MRPT_IS_BIG_ENDIAN
				for (uint32_t i=0;i<n;i++)
					in >> m_map[i].kf_mean >> m_map[i].dm_mean >> m_map[i].dmv_var_mean;
#else
				// Little endian: just read all at once:
				in.ReadBuffer( &m_map[0], sizeof(m_map[0])*m_map.size() );
#endif
			}

			// Version 1: Insertion options:
			if (version>=1)
			{
				uint8_t	i;
				in  >> i;
				m_mapType = TMapRepresentation(i);

				in	>> m_cov
					>> m_stackedCov;

				in  >> insertionOptions.sigma
					>> insertionOptions.cutoffRadius
					>> insertionOptions.R_min
					>> insertionOptions.R_max
					>> insertionOptions.KF_covSigma
					>> insertionOptions.KF_initialCellStd
					>> insertionOptions.KF_observationModelNoise
					>> insertionOptions.KF_defaultCellMeanValue
					>> insertionOptions.KF_W_size;
			}

			if (version>=3)
			{
				uint64_t N;
				in >> m_average_normreadings_mean >> m_average_normreadings_var >> N;
				m_average_normreadings_count = N;
			}

			m_hasToRecoverMeanAndCov = true;
		} break;
	default:
		MRPT_THROW_UNKNOWN_SERIALIZATION_VERSION(version)

	};

}

/*---------------------------------------------------------------
					TInsertionOptions
 ---------------------------------------------------------------*/
CWirelessPowerGridMap2D::TInsertionOptions::TInsertionOptions() :
	sensorType				( 0x0000 ),		//By default use the mean between all e-nose sensors

	sigma				( 0.15f ),
	cutoffRadius		( sigma * 3.0 ),
	R_min				( 0 ),
	R_max				( 3 ),
	dm_sigma_omega				( 0.05 ),  // See IROS 2009 paper (a scale parameter for the confidence)

	KF_covSigma					( 0.35f ),		// in meters
	KF_initialCellStd			( 1.0 ),		// std in normalized concentration units
	KF_observationModelNoise	( 0.25f ),		// in normalized concentration units
	KF_defaultCellMeanValue		( 0.25f ),
	KF_W_size					( 4 ),

	//MOSmodel parameters (only if useMOSmodel = true)
	useMOSmodel					( false ),
	tauR						( 4 ),			//Time constant for the rise phase
	tauD						( 4 ),
	lastObservations_size		( 5 ),
	winNoise_size				( 30 ),
	decimate_value				( 2 ),
	calibrated_tauD_voltages	( 0 ),			//Voltages values of the sensorID at the calibration points.
	calibrated_tauD_values		( 0 ),			//Tau_d
	calibrated_delay_RobotSpeeds ( 0 ),			//[m/s]
	calibrated_delay_values		( 0 ),			//Number of delayed samples before decimation
	enose_id					( 0 ),			//By default use the first enose
	save_maplog					( false )
	//VoltageDivider_Res			( 10000 ),
{
}

/*---------------------------------------------------------------
					dumpToTextStream
  ---------------------------------------------------------------*/
void  CWirelessPowerGridMap2D::TInsertionOptions::dumpToTextStream(CStream	&out) const
{
	out.printf("\n----------- [CWirelessPowerGridMap2D::TInsertionOptions] ------------ \n\n");
	out.printf("sigma                                   = %f\n", sigma);
	out.printf("cutoffRadius                            = %f\n", cutoffRadius);
	out.printf("R_min                                   = %f\n", R_min);
	out.printf("R_max                                   = %f\n", R_max);
	out.printf("KF_covSigma                             = %f\n", KF_covSigma);
	out.printf("KF_initialCellStd                       = %f\n", KF_initialCellStd);
	out.printf("KF_observationModelNoise                = %f\n", KF_observationModelNoise);
	out.printf("KF_defaultCellMeanValue                 = %f\n", KF_defaultCellMeanValue);
	out.printf("KF_W_size                               = %u\n", (unsigned)KF_W_size);
	out.printf("sensorType                           = %u\n", (unsigned)sensorType);
	out.printf("enose_id								= %u\n", (unsigned)enose_id);
	out.printf("useMOSmodel								= %c\n", useMOSmodel ? 'Y':'N' );
	//out.printf("VoltageDivider_Res						= %f\n", VoltageDivider_Res);
	out.printf("dm_sigma_omega	                        = %f\n", dm_sigma_omega);
	//MOSmodel parameters
	out.printf("tauR		                            = %f\n", tauR);
	out.printf("winNoise_size		                    = %u\n", (unsigned)winNoise_size);
	out.printf("decimate_value		                    = %u\n", (unsigned)decimate_value);
	out.printf("lastObservations_size                   = %u\n", (unsigned)lastObservations_size);

		//Need to dump the vector parameters (ToDo)

	out.printf("save_maplog		                        = %c\n", save_maplog ? 'Y':'N' );


	out.printf("\n");
}

/*---------------------------------------------------------------
					loadFromConfigFile
  ---------------------------------------------------------------*/
void  CWirelessPowerGridMap2D::TInsertionOptions::loadFromConfigFile(
	const mrpt::utils::CConfigFileBase  &iniFile,
	const std::string &section)
{
	sigma					= iniFile.read_float(section.c_str(),"sigma",sigma);
	cutoffRadius			= iniFile.read_float(section.c_str(),"cutoffRadius",cutoffRadius);
	R_min					= iniFile.read_float(section.c_str(),"R_min",R_min);
	R_max					= iniFile.read_float(section.c_str(),"R_max",R_max);
	KF_covSigma				= iniFile.read_float(section.c_str(),"KF_covSigma",KF_covSigma);
	KF_initialCellStd		= iniFile.read_float(section.c_str(),"KF_initialCellStd",KF_initialCellStd);
	KF_observationModelNoise= iniFile.read_float(section.c_str(),"KF_observationModelNoise",KF_observationModelNoise);
	KF_defaultCellMeanValue = iniFile.read_float(section.c_str(),"KF_defaultCellMeanValue",KF_defaultCellMeanValue);

	MRPT_LOAD_CONFIG_VAR(KF_W_size, int,   iniFile, section );

	{
		int tmpSensorType = iniFile.read_int(section.c_str(),"sensorType",-1);
		if (tmpSensorType>=0)
		{
			// Valid number found:
			sensorType = tmpSensorType;
		}
		else
		{ // fall back to old name, or default to current value:
			sensorType = iniFile.read_int(section.c_str(),"KF_sensorType",sensorType);
		}
	}

	enose_id				= iniFile.read_int(section.c_str(),"enose_id",enose_id);
	//VoltageDivider_Res		= iniFile.read_float(section.c_str(),"VoltageDivider_Res",VoltageDivider_Res);
	useMOSmodel				= iniFile.read_bool(section.c_str(),"useMOSmodel",useMOSmodel);

	//MOSmodel parameters

	tauR					= iniFile.read_float(section.c_str(),"tauR",tauR);
	winNoise_size			= iniFile.read_int(section.c_str(),"winNoise_size",winNoise_size);
	decimate_value			= iniFile.read_int(section.c_str(),"decimate_value",decimate_value);
	lastObservations_size	= iniFile.read_int(section.c_str(),"lastObservations_size",lastObservations_size);
	iniFile.read_vector(section.c_str(),"calibrated_tauD_voltages",calibrated_tauD_voltages,calibrated_tauD_voltages);
	iniFile.read_vector(section.c_str(),"calibrated_tauD_values",calibrated_tauD_values,calibrated_tauD_values);
	iniFile.read_vector(section.c_str(),"calibrated_delay_RobotSpeeds",calibrated_delay_RobotSpeeds,calibrated_delay_RobotSpeeds);
	iniFile.read_vector(section.c_str(),"calibrated_delay_values",calibrated_delay_values,calibrated_delay_values);
	save_maplog				= iniFile.read_bool(section.c_str(),"save_maplog",save_maplog);

	MRPT_LOAD_CONFIG_VAR(dm_sigma_omega, double,   iniFile, section );


	if (useMOSmodel)
	{
		//Update the values of delay according to decimation
		for (int i = 0; i<calibrated_delay_values.size(); i++){
			calibrated_delay_values[i] = round( calibrated_delay_values[i]/decimate_value );
		}
		//Get the lastObservations_size (Must be higher than max delay_value/decimate_value)
		if ( lastObservations_size < *max_element(calibrated_delay_values.begin(), calibrated_delay_values.end()) )
			lastObservations_size = *max_element( calibrated_delay_values.begin(), calibrated_delay_values.end() )+1 ;
	}
}


/*---------------------------------------------------------------
					saveAsBitmapFile
 ---------------------------------------------------------------*/
void  CWirelessPowerGridMap2D::saveAsBitmapFile(const std::string &filName) const
{
	MRPT_START;

	unsigned int	x,y;
	double c;
	const TWirelessPowerCell	*cell;

	mrpt::math::CMatrixDouble cells_mat(m_size_y,m_size_x);

	recoverMeanAndCov();	// Only has effects for KF2 method

	for (y=0;y<m_size_y;y++)
	{
		for (x=0;x<m_size_x;x++)
		{
			cell = cellByIndex(x,y);
			ASSERT_( cell!=NULL );

			switch (m_mapType)
			{
			case mrKernelDM:
			case mrKernelDMV:
				c = computeMeanCellValue_DM_DMV(cell);
				break;

			case mrKalmanFilter:
			case mrKalmanApproximate:
				c = cell->kf_mean;
				break;

			default:
				THROW_EXCEPTION("Unknown m_mapType!!");
			};

			if (c<0) c=0;
			if (c>1) c=1;

			cells_mat(m_size_y-1-y,x) = c;
		}
	}

	CImage	imgFl(cells_mat, true /* vals are normalized in [0,1] */);
	imgFl.saveToFile(filName);

	MRPT_END;
}

/*---------------------------------------------------------------
					resize
 ---------------------------------------------------------------*/
void  CWirelessPowerGridMap2D::resize(
	float	new_x_min,
	float	new_x_max,
	float	new_y_min,
	float	new_y_max,
	const TWirelessPowerCell& defaultValueNewCells,
	float	additionalMarginMeters)
{
	MRPT_START;

	size_t		old_sizeX = m_size_x;
	size_t		old_sizeY = m_size_y;
	float		old_x_min = m_x_min;
	float		old_y_min = m_y_min;

	// The parent class method:
	CDynamicGrid<TWirelessPowerCell>::resize(new_x_min,new_x_max,new_y_min,new_y_max,defaultValueNewCells,additionalMarginMeters);

	// Do we really resized?
	if ( m_size_x != old_sizeX || m_size_y != old_sizeY )
	{
		// YES:
		// If we are in a Kalman Filter representation, also build the new covariance matrix:
		if (m_mapType==mrKalmanFilter)
		{
			// ------------------------------------------
			//		Update the covariance matrix
			// ------------------------------------------
			size_t			i,j,N = m_size_y*m_size_x;	// The new number of cells
			CMatrixD		oldCov( m_cov );		// Make a copy

			//m_cov.saveToTextFile("__debug_cov_before_resize.txt");

			printf("[CWirelessPowerGridMap2D::resize] Resizing from %ux%u to %ux%u (%u cells)\n",
				static_cast<unsigned>(old_sizeX),
				static_cast<unsigned>(old_sizeY),
				static_cast<unsigned>(m_size_x),
				static_cast<unsigned>(m_size_y),
				static_cast<unsigned>(m_size_x*m_size_y) );

			m_cov.setSize( N,N );

			// Compute the new cells at the left and the bottom:
			size_t	Acx_left = round((old_x_min - m_x_min)/m_resolution);
			size_t	Acy_bottom = round((old_y_min - m_y_min)/m_resolution);

			// -------------------------------------------------------
			// STEP 1: Copy the old map values:
			// -------------------------------------------------------
			for (i = 0;i<N;i++)
			{
				size_t	cx1 = i % m_size_x;
				size_t	cy1 = i / m_size_x;

				bool	C1_isFromOldMap =	Acx_left<=cx1 && cx1<(Acx_left+old_sizeX) &&
											Acy_bottom<=cy1 && cy1<(Acy_bottom+old_sizeY);

				if ( C1_isFromOldMap )
				{
					for (j = i;j<N;j++)
					{
						size_t	cx2 = j % m_size_x;
						size_t	cy2 = j / m_size_x;

						bool	C2_isFromOldMap =	Acx_left<=cx2 && cx2<(Acx_left+old_sizeX) &&
													Acy_bottom<=cy2 && cy2<(Acy_bottom+old_sizeY);

						// Were both cells in the old map??? --> Copy it!
						if ( C1_isFromOldMap && C2_isFromOldMap )
						{
							// Copy values for the old matrix:
							unsigned int	idx_c1 = ( (cx1 - Acx_left) + old_sizeX * (cy1 - Acy_bottom ) );
							unsigned int	idx_c2 = ( (cx2 - Acx_left) + old_sizeX * (cy2 - Acy_bottom ) );

							MRPT_START;

							ASSERT_( cx1 >= Acx_left );
							ASSERT_( cy1 >= Acy_bottom );
							ASSERT_( (cx1 - Acx_left )<old_sizeX );
							ASSERT_( (cy1 - Acy_bottom )<old_sizeY );

							ASSERT_( cx2 >= Acx_left );
							ASSERT_( cy2 >= Acy_bottom );
							ASSERT_( (cx2 - Acx_left )<old_sizeX );
							ASSERT_( (cy2 - Acy_bottom )<old_sizeY );

							ASSERT_( idx_c1<old_sizeX*old_sizeY );
							ASSERT_( idx_c2<old_sizeX*old_sizeY );

							MRPT_END_WITH_CLEAN_UP( \
								printf("cx1=%u\n",static_cast<unsigned>(cx1)); \
								printf("cy1=%u\n",static_cast<unsigned>(cy1)); \
								printf("cx2=%u\n",static_cast<unsigned>(cx2)); \
								printf("cy2=%u\n",static_cast<unsigned>(cy2)); \
								printf("Acx_left=%u\n",static_cast<unsigned>(Acx_left)); \
								printf("Acy_bottom=%u\n",static_cast<unsigned>(Acy_bottom)); \
								printf("idx_c1=%u\n",static_cast<unsigned>(idx_c1)); \
								printf("idx_c2=%u\n",static_cast<unsigned>(idx_c2)); \
								);

							m_cov(i,j) = oldCov( idx_c1, idx_c2 );
							m_cov(j,i) = m_cov(i,j);

							if (i==j)
								ASSERT_( idx_c1 == idx_c2 );

							if (i==j && m_cov(i,i)<0)
							{
								printf("\ni=%u \nj=%i \ncx1,cy1 = %u,%u \n cx2,cy2=%u,%u \nidx_c1=%u \nidx_c2=%u \nAcx_left=%u \nAcy_bottom=%u \nold_sizeX=%u \n",
									static_cast<unsigned>(i),
									static_cast<unsigned>(j),
									static_cast<unsigned>(cx1),
									static_cast<unsigned>(cy1),
									static_cast<unsigned>(cx2),
									static_cast<unsigned>(cy2),
									static_cast<unsigned>(idx_c1),
									static_cast<unsigned>(idx_c2),
									static_cast<unsigned>(Acx_left),
									static_cast<unsigned>(Acy_bottom),
									static_cast<unsigned>(old_sizeX)
									);
							}
						}

						ASSERT_( !isNaN( m_cov(i,j) ) );

					} // for j
				}
			} // for i

			// -------------------------------------------------------
			// STEP 2: Set default values for new cells
			// -------------------------------------------------------
			for (i=0;i<N;i++)
			{
				size_t	cx1 = i % m_size_x;
				size_t	cy1 = i / m_size_x;

				bool	C1_isFromOldMap =	Acx_left<=cx1 && cx1<(Acx_left+old_sizeX) &&
											Acy_bottom<=cy1 && cy1<(Acy_bottom+old_sizeY);
				for (j=i;j<N;j++)
				{
					size_t	cx2 = j % m_size_x;
					size_t	cy2 = j / m_size_x;

					bool	C2_isFromOldMap =	Acx_left<=cx2 && cx2<(Acx_left+old_sizeX) &&
												Acy_bottom<=cy2 && cy2<(Acy_bottom+old_sizeY);
					double	dist=0;

					// If both cells were NOT in the old map??? --> Introduce default values:
					if ( !C1_isFromOldMap || !C2_isFromOldMap )
					{
						// Set a new starting value:
						if (i==j)
						{
							m_cov(i,i) = square(  insertionOptions.KF_initialCellStd );
						}
						else
						{
							dist = m_resolution*sqrt( static_cast<double>( square(cx1-cx2) +  square(cy1-cy2) ));
							double K = sqrt(m_cov(i,i)*m_cov(j,j));

							if ( isNaN( K ) )
							{
								printf("c(i,i)=%e   c(j,j)=%e\n",m_cov(i,i),m_cov(j,j));
								ASSERT_( !isNaN( K ) );
							}

							m_cov(i,j) = K * exp( -0.5 * square( dist/insertionOptions.KF_covSigma ) );
							m_cov(j,i) = m_cov(i,j);
						}

						ASSERT_( !isNaN( m_cov(i,j) ) );
					}
				} // for j
			} // for i


			//m_cov.saveToTextFile("__debug_cov_after_resize.txt");
			// Resize done!
			printf("[CWirelessPowerGridMap2D::resize] Done\n");

		} // end of Kalman Filter map
		else if (m_mapType==mrKalmanApproximate )
		{
			// ------------------------------------------
			//		Approximate-Kalman filter
			// ------------------------------------------

			// Cells with "std" == -1 are new ones, we have to change their std
			//   to "insertionOptions.KF_initialCellStd", then adapt appropriately
			//   the compressed cov. matrix:


			/*printf("[CWirelessPowerGridMap2D::resize] Resizing from %ux%u to %ux%u (%u cells)\n",
				static_cast<unsigned>(old_sizeX),
				static_cast<unsigned>(old_sizeY),
				static_cast<unsigned>(m_size_x),
				static_cast<unsigned>(m_size_y),
				static_cast<unsigned>(m_size_x*m_size_y) );*/

			// Adapt the size of the cov. matrix:
			const signed	W = insertionOptions.KF_W_size;
			const size_t	N = m_map.size();
			const size_t	K = 2*W*(W+1)+1;
			ASSERT_( K==m_stackedCov.getColCount() );
			ASSERT_( old_sizeX*old_sizeY== m_stackedCov.getRowCount() );

			// Compute the new cells at the left and the bottom:
			size_t	Acx_left = round((old_x_min - m_x_min)/m_resolution);
			size_t	Acy_bottom = round((old_y_min - m_y_min)/m_resolution);

			m_stackedCov.setSize( N,K );

			// Prepare the template for new cells:
			vector_double template_row(K);
			{
				const double	std0sqr	= square(  insertionOptions.KF_initialCellStd );
				double			*ptr	= &template_row[0];
				const double	res2	= square(m_resolution);
				const double	KF_covSigma2 = square(insertionOptions.KF_covSigma);

				// 1) current cell
				*ptr++ = std0sqr;

				// 2) W rest of the first row:
				int Acx, Acy = 0;
				for (Acx=1;Acx<=W;Acx++)
					*ptr++ = std0sqr * exp( -0.5 * (res2 * static_cast<double>(square(Acx) +  square(Acy)))/KF_covSigma2 );

				// 3) The others W rows:
				for (Acy=1;Acy<=W;Acy++)
					for (Acx=-W;Acx<=W;Acx++)
						*ptr++ = std0sqr * exp( -0.5 * (res2 * static_cast<double>(square(Acx) +  square(Acy)))/KF_covSigma2 );
			}

			// Go thru all the cells, from the bottom to the top so
			//  we don't need to make a temporary copy of the covariances:
			for (size_t i=N-1;i<N;i--) // i<N will become false for "i=-1" ;-)
			{
				int	cx,cy;
				idx2cxcy(i,cx,cy);

				const size_t			old_idx_of_i = (cx-Acx_left) + (cy-Acy_bottom)*old_sizeX;

				TWirelessPowerCell	&cell = m_map[i];
				if (cell.kf_std<0)
				{
					// "i" is a new cell, fix its std and fill out the compressed covariance:
					cell.kf_std = insertionOptions.KF_initialCellStd;

					double	*new_row = m_stackedCov.get_unsafe_row(i);
					memcpy( new_row,&template_row[0], sizeof(double)*K );
				}
				else
				{
					// "i" is an existing old cell: just copy the "m_stackedCov" row:
					ASSERT_(old_idx_of_i<m_stackedCov.getRowCount());
					if (old_idx_of_i!=i)		// Copy row only if it's moved
					{
						const double *ptr_old	= m_stackedCov.get_unsafe_row(old_idx_of_i);
						double	*ptr_new		= m_stackedCov.get_unsafe_row(i);
						memcpy( ptr_new,ptr_old, sizeof(double)*K );
					}
				}
			} // end for i
		} // end of Kalman-Approximate map
	}

	MRPT_END;
}


/*---------------------------------------------------------------
					insertObservation_KF
  ---------------------------------------------------------------*/
void  CWirelessPowerGridMap2D::insertObservation_KF(
	float			normReading,
	const CPose3D	&sensorPose_ )
{
	MRPT_START;

	const TWirelessPowerCell defCell(
				insertionOptions.KF_defaultCellMeanValue,	// mean
				insertionOptions.KF_initialCellStd			// std
				);

	const TPose3D sensorPose= TPose3D(sensorPose_);
	const TPose2D sensorPose2D = TPose2D( sensorPose );

	// Assure we have room enough in the grid!
	resize(	sensorPose2D.x - 1,
			sensorPose2D.x + 1,
			sensorPose2D.y - 1,
			sensorPose2D.y + 1,
			defCell );

	// --------------------------------------------------------
	// The Kalman-Filter estimation of the wifi grid-map:
	// --------------------------------------------------------

	// Prediction stage of KF:
	// ------------------------------------
	// Nothing to do here (static map)

	// Update stage of KF:
	//  We directly apply optimized formulas arising
	//   from our concrete sensor model.
	// -------------------------------------------------
	int						cellIdx = xy2idx( sensorPose.x, sensorPose.y );
	TWirelessPowerCell	*cell = cellByPos( sensorPose.x, sensorPose.y );
	ASSERT_(cell!=NULL);
	size_t					N,i,j;

	double		yk = normReading - cell->kf_mean;		// Predicted observation mean
	double		sk = m_cov(cellIdx,cellIdx) + square(insertionOptions.KF_observationModelNoise);	// Predicted observation variance
	double		sk_1 = 1.0 / sk;

	// The kalman gain:
//	CMatrixD		Kk;
//	vector_double	Kk;
//	m_cov.extractCol( cellIdx,Kk );
//	Kk *= 1.0/sk;

	//Kk.saveToTextFile("__debug_Kk.txt");

	std::vector<TWirelessPowerCell>::iterator	it;

#define WIFIGRIDMAP_VERBOSE		0

#if WIFIGRIDMAP_VERBOSE
	CTicTac		tictac;
	//cout << "[insertObservation_KF] Sensor: " << sensorPose2D << " measur: " << normReading << endl;
	printf("[insertObservation_KF] Updating mean values...");
	tictac.Tic();
#endif

	// Update mean values:
	// ---------------------------------------------------------
	for (i=0,it = m_map.begin();it!=m_map.end();it++,i++)
		//it->kf_mean =  it->kf_mean + yk * sk_1 * m_cov.get_unsafe(i,cellIdx);
		it->kf_mean +=  yk * sk_1 * m_cov(i,cellIdx);

#if WIFIGRIDMAP_VERBOSE
	printf("Done in %.03fms\n",	tictac.Tac()*1000 );
#endif

	// Update covariance matrix values:
	// ---------------------------------------------------------
	N = m_cov.getRowCount();

#if WIFIGRIDMAP_VERBOSE
	printf("[insertObservation_KF] Updating covariance matrix...");
	tictac.Tic();
#endif

	// We need to refer to the old cov: make an efficient copy of it:
	//CMatrixD		oldCov( m_cov );
	double	*oldCov	= (double*)/*mrpt_alloca*/malloc( sizeof(double)*N*N );
	double  *oldCov_ptr = oldCov;
	for (i=0;i<N;i++)
	{
		memcpy( oldCov_ptr, m_cov.get_unsafe_row(i) , sizeof(double)*N );
		oldCov_ptr+=N;
	}

//	m_cov.saveToTextFile("bef.txt");

#if WIFIGRIDMAP_VERBOSE
	printf("Copy matrix %ux%u: %.06fms\n",	(unsigned)m_cov.getRowCount(), (unsigned)m_cov.getColCount(),  tictac.Tac()*1000 );
#endif

	// The following follows from the expansion of Kalman Filter matrix equations
	// TODO: Add references to some paper (if any)?
	const double *oldCov_row_c = oldCov+cellIdx*N;
	for (i=0;i<N;i++)
	{
		const double oldCov_i_c = oldCov[i*N+cellIdx];
		const double sk_1_oldCov_i_c = sk_1 * oldCov_i_c;

		const double *oldCov_row_i = oldCov+i*N;
		for (j=i;j<N;j++)
		{
			double new_cov_ij = oldCov_row_i[j] - sk_1_oldCov_i_c * oldCov_row_c[j];

			// Make symmetric:
			m_cov.set_unsafe(i,j, new_cov_ij );
			m_cov.set_unsafe(j,i, new_cov_ij );

			// Update the "std" in the cell as well:
			if (i==j)
			{
				if (m_cov(i,i)<0){
					printf("fallo");
				}

				ASSERT_( m_cov(i,i)>=0 );
				m_map[ i ].kf_std = sqrt( new_cov_ij );
			}
			//ASSERT_( !isNaN( m_cov(i,j) ) );

		} // j
	} // i

	// Free mem:
	/*mrpt_alloca_*/ free( oldCov );

#if WIFIGRIDMAP_VERBOSE
	printf("Done! %.03fms\n",	tictac.Tac()*1000 );
#endif

//	m_cov.saveToTextFile("aft.txt");
	//mrpt::system::pause();

	MRPT_END;
}


/*---------------------------------------------------------------
					saveMetricMapRepresentationToFile
  ---------------------------------------------------------------*/
void  CWirelessPowerGridMap2D::saveMetricMapRepresentationToFile(
	const std::string	&filNamePrefix ) const
{
	std::string		fil;

	// Save as a bitmap:
	fil = filNamePrefix + std::string("_mean.png");
	saveAsBitmapFile( fil );

	if ( m_mapType == mrKernelDM || m_mapType == mrKernelDMV )
	{
		CMatrix  all_means(m_size_y,m_size_x);
		CMatrix  all_vars(m_size_y,m_size_x);

		for (size_t y=0;y<m_size_y;y++)
			for (size_t x=0;x<m_size_x;x++)
			{
				const TWirelessPowerCell * cell =cellByIndex(x,y);
				all_means(y,x) = computeMeanCellValue_DM_DMV(cell);
				all_vars(y,x)  = computeVarCellValue_DM_DMV(cell);
			}

		all_means.saveToTextFile( filNamePrefix + std::string("_mean.txt"), MATRIX_FORMAT_FIXED );
		if (m_mapType == mrKernelDMV)
			all_vars.saveToTextFile( filNamePrefix + std::string("_var.txt"), MATRIX_FORMAT_FIXED );
	}

	if ( m_mapType == mrKalmanApproximate )
	{
		m_stackedCov.saveToTextFile( filNamePrefix + std::string("_mean_compressed_cov.txt"), MATRIX_FORMAT_FIXED );
	}

	if ( m_mapType == mrKalmanFilter || m_mapType == mrKalmanApproximate )
	{
		recoverMeanAndCov();

		// Dimensions:
		CMatrix DIMs(1,4);
		DIMs(0,0)=m_x_min;
		DIMs(0,1)=m_x_max;
		DIMs(0,2)=m_y_min;
		DIMs(0,3)=m_y_max;

		DIMs.saveToTextFile( filNamePrefix + std::string("_grid_limits.txt"), MATRIX_FORMAT_FIXED );

		// Save the mean matrix:
		CMatrix	MEAN( m_size_y,m_size_x);
		CMatrix	STDs( m_size_y, m_size_x );

		for (size_t i=0;i<m_size_y;i++)
			for (size_t j=0;j<m_size_x;j++)
			{
				MEAN(i,j)=cellByIndex(j,i)->kf_mean;
				STDs(i,j)=cellByIndex(j,i)->kf_std;
			}

		MEAN.saveToTextFile( filNamePrefix + std::string("_mean.txt"), MATRIX_FORMAT_FIXED );
		STDs.saveToTextFile( filNamePrefix + std::string("_cells_std.txt"), MATRIX_FORMAT_FIXED );

		if (m_mapType == mrKalmanFilter)
		{
			// Save the covariance matrix:
			m_cov.saveToTextFile( filNamePrefix + std::string("_mean_cov.txt") );
		}

		// And also as bitmap:
		STDs.normalize();
		CImage	img_cov(STDs, true);
		img_cov.saveToFile(filNamePrefix + std::string("_cells_std.png"), true /* vertical flip */);

		// Save the 3D graphs:
		saveAsMatlab3DGraph( filNamePrefix + std::string("_3D.m") );
	}

}

/*---------------------------------------------------------------
					saveAsMatlab3DGraph
  ---------------------------------------------------------------*/
void  CWirelessPowerGridMap2D::saveAsMatlab3DGraph(const std::string  &filName) const
{
	MRPT_START;

	const double std_times = 3;

	ASSERT_( m_mapType == mrKalmanFilter || m_mapType==mrKalmanApproximate );

	recoverMeanAndCov();

	FILE	*f= os::fopen( filName.c_str(), "wt" );
	if (!f)
		THROW_EXCEPTION("Couldn't create output file!");

	os::fprintf(f,"%%-------------------------------------------------------\n");
	os::fprintf(f,"%% File automatically generated using the MRPT method:\n");
	os::fprintf(f,"%%'CWirelessPowerGridMap2D::saveAsMatlab3DGraph'\n");
	os::fprintf(f,"%%\n");
	os::fprintf(f,"%%                        ~ MRPT ~\n");
	os::fprintf(f,"%%  Jose Luis Blanco Claraco, University of Malaga @ 2006-2007\n");
	os::fprintf(f,"%%  http://www.isa.uma.es/ \n");
	os::fprintf(f,"%%-------------------------------------------------------\n\n");


	unsigned int	cx,cy;
	vector<float>	xs,ys;

	// xs: array of X-axis values
	os::fprintf(f,"xs = [");
	xs.resize( m_size_x );
	for (cx=0;cx<m_size_x;cx++)
	{
		xs[cx] = m_x_min + m_resolution * cx;
		os::fprintf(f,"%f ", xs[cx]);
	}
	os::fprintf(f,"];\n");

	// ys: array of X-axis values
	os::fprintf(f,"ys = [");
	ys.resize( m_size_y );
	for (cy=0;cy<m_size_y;cy++)
	{
		ys[cy] = m_y_min + m_resolution * cy;
		os::fprintf(f,"%f ", ys[cy]);
	}
	os::fprintf(f,"];\n");

	// z_mean: matrix with mean concentration values
	os::fprintf(f,"z_mean = [\n");
	for (cy=0;cy<m_size_y;cy++)
	{
		for (cx=0;cx<m_size_x;cx++)
		{
            const TWirelessPowerCell	*cell = cellByIndex( cx,cy );
			ASSERT_( cell!=NULL );
			os::fprintf(f,"%e ", cell->kf_mean  );
		} // for cx

		if (cy<(m_size_y-1))
			os::fprintf(f,"; ...\n");
	} // for cy
	os::fprintf(f,"];\n\n");

	// z_upper: matrix with upper confidence levels for concentration values
	os::fprintf(f,"z_upper = [\n");
	for (cy=0;cy<m_size_y;cy++)
	{
		for (cx=0;cx<m_size_x;cx++)
		{
            const TWirelessPowerCell	*cell = cellByIndex( cx,cy );
			ASSERT_( cell!=NULL );
			//os::fprintf(f,"%e ",  min(1.0,max(0.0,  cell->kf_mean + std_times * cell->kf_std ) ));
			os::fprintf(f,"%e ",  max(0.0,  cell->kf_mean + std_times * cell->kf_std ));
		} // for cx

		if (cy<(m_size_y-1))
			os::fprintf(f,"; ...\n");
	} // for cy
	os::fprintf(f,"];\n\n");

	// z_lowe: matrix with lower confidence levels for concentration values
	os::fprintf(f,"z_lower = [\n");
	for (cy=0;cy<m_size_y;cy++)
	{
		for (cx=0;cx<m_size_x;cx++)
		{
			const TWirelessPowerCell	*cell = cellByIndex( cx,cy );
			ASSERT_(cell!=NULL);

			os::fprintf(f,"%e ",  min(1.0,max(0.0,  cell->kf_mean - std_times * cell->kf_std ) ));
		} // for cx

		if (cy<(m_size_y-1))
			os::fprintf(f,"; ...\n");
	} // for cy
	os::fprintf(f,"];\n\n");

	// Plot them:
	os::fprintf(f,"hold off;\n");
	os::fprintf(f,"S1 = surf(xs,ys,z_mean);\n");
	os::fprintf(f,"hold on;\n");
	os::fprintf(f,"S2 = surf(xs,ys,z_upper);\n");
	os::fprintf(f,"S3 = surf(xs,ys,z_lower);\n");
	os::fprintf(f,"\n");
	os::fprintf(f,"set(S1,'FaceAlpha',0.9,'EdgeAlpha',0.9);\n");
	os::fprintf(f,"set(S2,'FaceAlpha',0.4,'EdgeAlpha',0.4);\n");
	os::fprintf(f,"set(S3,'FaceAlpha',0.2,'EdgeAlpha',0.2);\n");
	os::fprintf(f,"\n");
	os::fprintf(f,"set(gca,'PlotBoxAspectRatio',[%f %f %f]);\n",
		m_x_max-m_x_min,
		m_y_max-m_y_min,
		4.0 );
	os::fprintf(f,"view(-40,30)\n");
	os::fprintf(f,"axis([%f %f %f %f 0 1]);\n",
		m_x_min,
		m_x_max,
		m_y_min,
		m_y_max
		);


	fclose(f);

	MRPT_END;

}

/*---------------------------------------------------------------
						getAs3DObject
---------------------------------------------------------------*/
void  CWirelessPowerGridMap2D::getAs3DObject( mrpt::opengl::CSetOfObjectsPtr	&outObj ) const
{
	if (m_disableSaveAs3DObject)
		return;

	recoverMeanAndCov();

	opengl::CSetOfTriangles::TTriangle		triag;

	unsigned int	cx,cy;
	vector<float>	xs,ys;

	// xs: array of X-axis values
	xs.resize( m_size_x );
	for (cx=0;cx<m_size_x;cx++)	xs[cx] = m_x_min + m_resolution * cx;

	// ys: array of X-axis values
	ys.resize( m_size_y );
	for (cy=0;cy<m_size_y;cy++)	ys[cy] = m_y_min + m_resolution * cy;

	// Draw the surfaces:
	switch(m_mapType)
	{
	case mrKalmanFilter:
	case mrKalmanApproximate:
		{
			opengl::CSetOfTrianglesPtr obj = opengl::CSetOfTriangles::Create();
			const double  std_times = 2;
			const double  z_aspect_ratio = 4;

			//  Compute max/min values:
			// ---------------------------------------
			double	maxVal=0, minVal=1, AMaxMin;
			for (cy=1;cy<m_size_y;cy++)
			{
				for (cx=1;cx<m_size_x;cx++)
				{
					const TWirelessPowerCell	*cell_xy = cellByIndex( cx,cy ); ASSERT_( cell_xy!=NULL );
					minVal = min(minVal, cell_xy->kf_mean);
					maxVal = max(maxVal, cell_xy->kf_mean);
				}
			}

			AMaxMin = maxVal - minVal;
			if (AMaxMin==0) AMaxMin=1;


			// ---------------------------------------
			//  BOTTOM LAYER:  mean - K*std
			// ---------------------------------------
			triag.a[0]=triag.a[1]=triag.a[2]= 0.8f;

			for (cy=1;cy<m_size_y;cy++)
			{
				for (cx=1;cx<m_size_x;cx++)
				{
					// Cell values:
					const TWirelessPowerCell	*cell_xy = cellByIndex( cx,cy ); ASSERT_( cell_xy!=NULL );
					const TWirelessPowerCell	*cell_x_1y = cellByIndex( cx-1,cy ); ASSERT_( cell_x_1y!=NULL );
					const TWirelessPowerCell	*cell_xy_1 = cellByIndex( cx,cy-1 ); ASSERT_( cell_xy_1!=NULL );
					const TWirelessPowerCell	*cell_x_1y_1 = cellByIndex( cx-1,cy-1 ); ASSERT_( cell_x_1y_1!=NULL );

					double c_xy	= min(1.0,max(0.0, cell_xy->kf_mean - std_times*cell_xy->kf_std ) );
					double c_x_1y	= min(1.0,max(0.0, cell_x_1y->kf_mean - std_times*cell_x_1y->kf_std ) );
					double c_xy_1	= min(1.0,max(0.0, cell_xy_1->kf_mean - std_times*cell_xy_1->kf_std ) );
					double c_x_1y_1= min(1.0,max(0.0, cell_x_1y_1->kf_mean - std_times*cell_x_1y_1->kf_std) );

					double col_xy		= min(1.0,max(0.0, (cell_xy->kf_mean-minVal)/AMaxMin ) );
					double col_x_1y	= min(1.0,max(0.0, (cell_x_1y->kf_mean-minVal)/AMaxMin ) );
					double col_xy_1	= min(1.0,max(0.0, (cell_xy_1->kf_mean-minVal)/AMaxMin ) );
					double col_x_1y_1	= min(1.0,max(0.0, (cell_x_1y_1->kf_mean-minVal)/AMaxMin ) );

					// Triangle #1:
					//if ( fabs(c_xy-0.5f)<0.49f ||
					//	 fabs(c_x_1y-0.5f)<0.49f ||
					//	 fabs(c_xy_1-0.5f)<0.49f ||
					//	 fabs(c_x_1y_1-0.5f)<0.49f )
					{
						triag.x[0] = xs[cx];	triag.y[0] = ys[cy];	triag.z[0] = c_xy;
						triag.x[1] = xs[cx];	triag.y[1] = ys[cy-1];	triag.z[1] = c_xy_1;
						triag.x[2] = xs[cx-1];	triag.y[2] = ys[cy-1];	triag.z[2] = c_x_1y_1;
						jet2rgb( col_xy,triag.r[0],triag.g[0],triag.b[0] );
						jet2rgb( col_xy_1,triag.r[1],triag.g[1],triag.b[1] );
						jet2rgb( col_x_1y_1,triag.r[2],triag.g[2],triag.b[2] );

						obj->insertTriangle( triag );
					}

					// Triangle #2:
					//if ( fabs(c_xy-0.5f)<0.49f ||
					//	 fabs(c_x_1y-0.5f)<0.49f ||
					//	 fabs(c_xy_1-0.5f)<0.49f ||
					//	 fabs(c_x_1y_1-0.5f)<0.49f )
					{
						triag.x[0] = xs[cx];	triag.y[0] = ys[cy];	triag.z[0] = c_xy;
						triag.x[1] = xs[cx-1];	triag.y[1] = ys[cy-1];	triag.z[1] = c_x_1y_1;
						triag.x[2] = xs[cx-1];	triag.y[2] = ys[cy];	triag.z[2] = c_x_1y;

						jet2rgb( col_xy,triag.r[0],triag.g[0],triag.b[0] );
						jet2rgb( col_x_1y_1,triag.r[1],triag.g[1],triag.b[1] );
						jet2rgb( col_x_1y,triag.r[2],triag.g[2],triag.b[2] );

						obj->insertTriangle( triag );
					}
				} // for cx
			} // for cy
	/**/
			// ---------------------------------------
			//  MID LAYER:  mean
			// ---------------------------------------
	/**/
			triag.a[0]=triag.a[1]=triag.a[2]= 0.8f;
			for (cy=1;cy<m_size_y;cy++)
			{
				for (cx=1;cx<m_size_x;cx++)
				{
					// Cell values:
					const TWirelessPowerCell	*cell_xy = cellByIndex( cx,cy ); ASSERT_( cell_xy!=NULL );
					const TWirelessPowerCell	*cell_x_1y = cellByIndex( cx-1,cy ); ASSERT_( cell_x_1y!=NULL );
					const TWirelessPowerCell	*cell_xy_1 = cellByIndex( cx,cy-1 ); ASSERT_( cell_xy_1!=NULL );
					const TWirelessPowerCell	*cell_x_1y_1 = cellByIndex( cx-1,cy-1 ); ASSERT_( cell_x_1y_1!=NULL );

					double c_xy	= min(1.0,max(0.0, cell_xy->kf_mean ) );
					double c_x_1y	= min(1.0,max(0.0, cell_x_1y->kf_mean ) );
					double c_xy_1	= min(1.0,max(0.0, cell_xy_1->kf_mean ) );
					double c_x_1y_1= min(1.0,max(0.0, cell_x_1y_1->kf_mean ) );

					double col_xy		= min(1.0,max(0.0, cell_xy->kf_mean ) );
					double col_xy_1	= min(1.0,max(0.0, cell_xy_1->kf_mean ) );
					double col_x_1y	= min(1.0,max(0.0, cell_x_1y->kf_mean ) );
					double col_x_1y_1	= min(1.0,max(0.0, cell_x_1y_1->kf_mean ) );

					// Triangle #1:
					triag.x[0] = xs[cx];	triag.y[0] = ys[cy];	triag.z[0] = z_aspect_ratio*c_xy;
					triag.x[1] = xs[cx];	triag.y[1] = ys[cy-1];	triag.z[1] = z_aspect_ratio*c_xy_1;
					triag.x[2] = xs[cx-1];	triag.y[2] = ys[cy-1];	triag.z[2] = z_aspect_ratio*c_x_1y_1;
					jet2rgb( col_xy,triag.r[0],triag.g[0],triag.b[0] );
					jet2rgb( col_xy_1,triag.r[1],triag.g[1],triag.b[1] );
					jet2rgb( col_x_1y_1,triag.r[2],triag.g[2],triag.b[2] );

					obj->insertTriangle( triag );

					// Triangle #2:
					triag.x[0] = xs[cx];	triag.y[0] = ys[cy];	triag.z[0] = z_aspect_ratio*c_xy;
					triag.x[1] = xs[cx-1];	triag.y[1] = ys[cy-1];	triag.z[1] = z_aspect_ratio*c_x_1y_1;
					triag.x[2] = xs[cx-1];	triag.y[2] = ys[cy];	triag.z[2] = z_aspect_ratio*c_x_1y;

					jet2rgb( col_xy,triag.r[0],triag.g[0],triag.b[0] );
					jet2rgb( col_x_1y_1,triag.r[1],triag.g[1],triag.b[1] );
					jet2rgb( col_x_1y,triag.r[2],triag.g[2],triag.b[2] );
					obj->insertTriangle( triag );

				} // for cx
			} // for cy
	/**/

			// ---------------------------------------
			//  TOP LAYER:  mean + K*std
			// ---------------------------------------
			triag.a[0]=triag.a[1]=triag.a[2]= 0.5f;

			for (cy=1;cy<m_size_y;cy++)
			{
				for (cx=1;cx<m_size_x;cx++)
				{
					// Cell values:
					const TWirelessPowerCell	*cell_xy = cellByIndex( cx,cy ); ASSERT_( cell_xy!=NULL );
					const TWirelessPowerCell	*cell_x_1y = cellByIndex( cx-1,cy ); ASSERT_( cell_x_1y!=NULL );
					const TWirelessPowerCell	*cell_xy_1 = cellByIndex( cx,cy-1 ); ASSERT_( cell_xy_1!=NULL );
					const TWirelessPowerCell	*cell_x_1y_1 = cellByIndex( cx-1,cy-1 ); ASSERT_( cell_x_1y_1!=NULL );

					double c_xy	= min(1.0,max(0.0, cell_xy->kf_mean + std_times*cell_xy->kf_std ) );
					double c_x_1y	= min(1.0,max(0.0, cell_x_1y->kf_mean + std_times*cell_x_1y->kf_std ) );
					double c_xy_1	= min(1.0,max(0.0, cell_xy_1->kf_mean + std_times*cell_xy_1->kf_std ) );
					double c_x_1y_1= min(1.0,max(0.0, cell_x_1y_1->kf_mean + std_times*cell_x_1y_1->kf_std) );

					double col_xy		= min(1.0,max(0.0, (cell_xy->kf_mean-minVal)/AMaxMin ) );
					double col_x_1y	= min(1.0,max(0.0, (cell_x_1y->kf_mean-minVal)/AMaxMin ) );
					double col_xy_1	= min(1.0,max(0.0, (cell_xy_1->kf_mean-minVal)/AMaxMin ) );
					double col_x_1y_1	= min(1.0,max(0.0, (cell_x_1y_1->kf_mean-minVal)/AMaxMin ) );

					// Triangle #1:
					/*if ( fabs(c_xy-0.5f)<0.49f ||
						 fabs(c_x_1y-0.5f)<0.49f ||
						 fabs(c_xy_1-0.5f)<0.49f ||
						 fabs(c_x_1y_1-0.5f)<0.49f )*/
					{
						triag.x[0] = xs[cx];	triag.y[0] = ys[cy];	triag.z[0] = z_aspect_ratio*c_xy;
						triag.x[1] = xs[cx];	triag.y[1] = ys[cy-1];	triag.z[1] = z_aspect_ratio*c_xy_1;
						triag.x[2] = xs[cx-1];	triag.y[2] = ys[cy-1];	triag.z[2] = z_aspect_ratio*c_x_1y_1;
						jet2rgb( col_xy,triag.r[0],triag.g[0],triag.b[0] );
						jet2rgb( col_xy_1,triag.r[1],triag.g[1],triag.b[1] );
						jet2rgb( col_x_1y_1,triag.r[2],triag.g[2],triag.b[2] );

						obj->insertTriangle( triag );
					}

					// Triangle #2:
					/*if ( fabs(c_xy-0.5f)<0.49f ||
						 fabs(c_x_1y-0.5f)<0.49f ||
						 fabs(c_xy_1-0.5f)<0.49f ||
						 fabs(c_x_1y_1-0.5f)<0.49f )*/
					{
						triag.x[0] = xs[cx];	triag.y[0] = ys[cy];	triag.z[0] = z_aspect_ratio*c_xy;
						triag.x[1] = xs[cx-1];	triag.y[1] = ys[cy-1];	triag.z[1] = z_aspect_ratio*c_x_1y_1;
						triag.x[2] = xs[cx-1];	triag.y[2] = ys[cy];	triag.z[2] = z_aspect_ratio*c_x_1y;

						jet2rgb( col_xy,triag.r[0],triag.g[0],triag.b[0] );
						jet2rgb( col_x_1y_1,triag.r[1],triag.g[1],triag.b[1] );
						jet2rgb( col_x_1y,triag.r[2],triag.g[2],triag.b[2] );

						obj->insertTriangle( triag );
					}
				} // for cx
			} // for cy


			obj->enableTransparency(true);;
			outObj->insert( obj );
		}
		break; // end KF models

	case mrKernelDM:
	case mrKernelDMV:
		{
			// Draw for Kernel model:
			// ----------------------------------
			opengl::CSetOfTrianglesPtr obj = opengl::CSetOfTriangles::Create();
			obj->enableTransparency(false);

			//  Compute max/min values:
			// ---------------------------------------
			double 	maxVal=0, minVal=1, AMaxMin;
			double c;
			for (cy=1;cy<m_size_y;cy++)
			{
				for (cx=1;cx<m_size_x;cx++)
				{
					const TWirelessPowerCell	*cell_xy = cellByIndex( cx,cy ); ASSERT_( cell_xy!=NULL );
					c = computeMeanCellValue_DM_DMV( cell_xy );
					minVal = min(minVal, c);
					maxVal = max(maxVal, c);
				}
			}

			AMaxMin = maxVal - minVal;
			if (AMaxMin==0) AMaxMin=1;

			// ---------------------------------------
			//  MID LAYER:  mean
			// ---------------------------------------
			triag.a[0]=triag.a[1]=triag.a[2]= 0.75f;	// alpha (transparency)
			for (cy=1;cy<m_size_y;cy++)
			{
				for (cx=1;cx<m_size_x;cx++)
				{
					// Cell values:
					const TWirelessPowerCell	*cell_xy = cellByIndex( cx,cy ); ASSERT_( cell_xy!=NULL );
					const TWirelessPowerCell	*cell_x_1y = cellByIndex( cx-1,cy ); ASSERT_( cell_x_1y!=NULL );
					const TWirelessPowerCell	*cell_xy_1 = cellByIndex( cx,cy-1 ); ASSERT_( cell_xy_1!=NULL );
					const TWirelessPowerCell	*cell_x_1y_1 = cellByIndex( cx-1,cy-1 ); ASSERT_( cell_x_1y_1!=NULL );

					double c_xy	= min(1.0,max(0.0, computeMeanCellValue_DM_DMV(cell_xy) ) );
					double c_x_1y	= min(1.0,max(0.0, computeMeanCellValue_DM_DMV(cell_x_1y) ) );
					double c_xy_1	= min(1.0,max(0.0, computeMeanCellValue_DM_DMV(cell_xy_1) ) );
					double c_x_1y_1= min(1.0,max(0.0, computeMeanCellValue_DM_DMV(cell_x_1y_1) ) );

					double col_xy		= c_xy;
					double col_xy_1	= c_xy_1;
					double col_x_1y	= c_x_1y;
					double col_x_1y_1	= c_x_1y_1;

			/*		// Triangle #1:
					triag.x[0] = xs[cx];	triag.y[0] = ys[cy];	triag.z[0] = c_xy;
					triag.x[1] = xs[cx];	triag.y[1] = ys[cy-1];	triag.z[1] = c_xy_1;
					triag.x[2] = xs[cx-1];	triag.y[2] = ys[cy-1];	triag.z[2] = c_x_1y_1;
					jet2rgb( col_xy,triag.r[0],triag.g[0],triag.b[0] );
					jet2rgb( col_xy_1,triag.r[1],triag.g[1],triag.b[1] );
					jet2rgb( col_x_1y_1,triag.r[2],triag.g[2],triag.b[2] );

					obj->insertTriangle( triag );

					// Triangle #2:
					triag.x[0] = xs[cx];	triag.y[0] = ys[cy];	triag.z[0] = c_xy;
					triag.x[1] = xs[cx-1];	triag.y[1] = ys[cy-1];	triag.z[1] = c_x_1y_1;
					triag.x[2] = xs[cx-1];	triag.y[2] = ys[cy];	triag.z[2] = c_x_1y;

					jet2rgb( col_xy,triag.r[0],triag.g[0],triag.b[0] );
					jet2rgb( col_x_1y_1,triag.r[1],triag.g[1],triag.b[1] );
					jet2rgb( col_x_1y,triag.r[2],triag.g[2],triag.b[2] );
					obj->insertTriangle( triag );*/

					// Triangle #1:
					if ( cell_xy->dm_mean_w>0 && cell_xy_1->dm_mean_w>0 && cell_x_1y_1->dm_mean_w>0)
					{
						triag.x[0] = xs[cx];           triag.y[0] = ys[cy];           triag.z[0] = c_xy;
						triag.x[1] = xs[cx];           triag.y[1] = ys[cy-1];       triag.z[1] = c_xy_1;
						triag.x[2] = xs[cx-1];       triag.y[2] = ys[cy-1];       triag.z[2] = c_x_1y_1;
						jet2rgb( col_xy,triag.r[0],triag.g[0],triag.b[0] );
						jet2rgb( col_xy_1,triag.r[1],triag.g[1],triag.b[1] );
						jet2rgb( col_x_1y_1,triag.r[2],triag.g[2],triag.b[2] );
						obj->insertTriangle( triag );
					}else if ( cell_xy->dm_mean_w>0 || cell_xy_1->dm_mean_w>0 || cell_x_1y_1->dm_mean_w>0){         // Non_observed_cells_default_values
						triag.x[0] = xs[cx];           triag.y[0] = ys[cy];           triag.z[0] = c_xy/1.0;
						triag.x[1] = xs[cx];           triag.y[1] = ys[cy-1];       triag.z[1] = c_xy_1/1.0;
						triag.x[2] = xs[cx-1];       triag.y[2] = ys[cy-1];       triag.z[2] = c_x_1y_1/1.0;
						jet2rgb( col_xy,triag.r[0],triag.g[0],triag.b[0] );
						jet2rgb( col_xy_1,triag.r[1],triag.g[1],triag.b[1] );
						jet2rgb( col_x_1y_1,triag.r[2],triag.g[2],triag.b[2] );

						obj->insertTriangle( triag );
					}

					// Triangle #2:
					if ( cell_xy->dm_mean_w>0 && cell_x_1y_1->dm_mean_w>0 && cell_x_1y->dm_mean_w>0)
					{
						triag.x[0] = xs[cx];           triag.y[0] = ys[cy];           triag.z[0] = c_xy;
						triag.x[1] = xs[cx-1];       triag.y[1] = ys[cy-1];       triag.z[1] = c_x_1y_1;
						triag.x[2] = xs[cx-1];       triag.y[2] = ys[cy];           triag.z[2] = c_x_1y;
						jet2rgb( col_xy,triag.r[0],triag.g[0],triag.b[0] );
						jet2rgb( col_x_1y_1,triag.r[1],triag.g[1],triag.b[1] );
						jet2rgb( col_x_1y,triag.r[2],triag.g[2],triag.b[2] );
						obj->insertTriangle( triag );
					}else if ( cell_xy->dm_mean_w>0 || cell_x_1y_1->dm_mean_w>0 || cell_x_1y->dm_mean_w>0){ // Non_observed_cells_default_values
						triag.x[0] = xs[cx];           triag.y[0] = ys[cy];           triag.z[0] = c_xy/1.0;
						triag.x[1] = xs[cx-1];       triag.y[1] = ys[cy-1];       triag.z[1] = c_x_1y_1/1.0;
						triag.x[2] = xs[cx-1];       triag.y[2] = ys[cy];           triag.z[2] = c_x_1y/1.0;
						jet2rgb( col_xy,triag.r[0],triag.g[0],triag.b[0] );
						jet2rgb( col_x_1y_1,triag.r[1],triag.g[1],triag.b[1] );
						jet2rgb( col_x_1y,triag.r[2],triag.g[2],triag.b[2] );
						obj->insertTriangle( triag );
					}





				} // for cx
			} // for cy
			outObj->insert( obj );
		}
		break; // end Kernel models
	}; // end switch maptype
}

/*---------------------------------------------------------------
  Computes the ratio in [0,1] of correspondences between "this" and the "otherMap" map, whose 6D pose relative to "this" is "otherMapPose"
 *   In the case of a multi-metric map, this returns the average between the maps. This method always return 0 for grid maps.
 * \param  otherMap					  [IN] The other map to compute the matching with.
 * \param  otherMapPose				  [IN] The 6D pose of the other map as seen from "this".
 * \param  minDistForCorr			  [IN] The minimum distance between 2 non-probabilistic map elements for counting them as a correspondence.
 * \param  minMahaDistForCorr		  [IN] The minimum Mahalanobis distance between 2 probabilistic map elements for counting them as a correspondence.
 *
 * \return The matching ratio [0,1]
 * \sa computeMatchingWith2D
  --------------------------------------------------------------- */
float  CWirelessPowerGridMap2D::compute3DMatchingRatio(
		const CMetricMap						*otherMap,
		const CPose3D							&otherMapPose,
		float									minDistForCorr,
		float									minMahaDistForCorr
		) const
{
	MRPT_UNUSED_PARAM(otherMap);
	MRPT_UNUSED_PARAM(otherMapPose);
	MRPT_UNUSED_PARAM(minDistForCorr);
	MRPT_UNUSED_PARAM(minMahaDistForCorr);

	return 0;
}


/*---------------------------------------------------------------
					computeMeanCellValue_DM_DMV
 ---------------------------------------------------------------*/
double  CWirelessPowerGridMap2D::computeMeanCellValue_DM_DMV (const TWirelessPowerCell *cell ) const
{
	// A confidence measure:
	const double alpha = 1.0 - std::exp(-square(cell->dm_mean_w/insertionOptions.dm_sigma_omega));
	const double r_val = (cell->dm_mean_w>0) ? (cell->dm_mean / cell->dm_mean_w) : 0;
	return alpha * r_val + (1-alpha) * 0 ; //m_average_normreadings_mean;
}

/*---------------------------------------------------------------
					computeVarCellValue_DM_DMV
 ---------------------------------------------------------------*/
double CWirelessPowerGridMap2D::computeVarCellValue_DM_DMV (const TWirelessPowerCell *cell ) const
{
	// A confidence measure:
	const double alpha = 1.0 - std::exp(-square(cell->dm_mean_w/insertionOptions.dm_sigma_omega));
	const double r_val = (cell->dm_mean_w>0) ? (cell->dmv_var_mean / cell->dm_mean_w) : 0;
	return alpha * r_val + (1-alpha) * m_average_normreadings_var;
}

/*---------------------------------------------------------------
					predictMeasurement
  ---------------------------------------------------------------*/
void CWirelessPowerGridMap2D::predictMeasurement(
	const double	&x,
	const double	&y,
	double			&out_predict_response,
	double			&out_predict_response_variance )
{
	MRPT_START

	switch (m_mapType)
	{
	case mrKernelDM:
		{
			TWirelessPowerCell	*cell = cellByPos( x, y );
			if (!cell)
			{
				out_predict_response = m_average_normreadings_mean;
				out_predict_response_variance = square( insertionOptions.KF_initialCellStd );
			}
			else
			{
				out_predict_response = computeMeanCellValue_DM_DMV(cell);
				out_predict_response_variance = square( insertionOptions.KF_initialCellStd );
			}
		}
		break;
	case mrKernelDMV:
		{
			TWirelessPowerCell	*cell = cellByPos( x, y );
			if (!cell)
			{
				out_predict_response = m_average_normreadings_mean;
				out_predict_response_variance = square( insertionOptions.KF_initialCellStd );
			}
			else
			{
				out_predict_response = computeMeanCellValue_DM_DMV(cell);
				out_predict_response_variance = computeVarCellValue_DM_DMV(cell);
			}
		}
		break;

	case mrKalmanFilter:
	case mrKalmanApproximate:
		{
			if (m_hasToRecoverMeanAndCov)	recoverMeanAndCov();	// Just for KF2

			TWirelessPowerCell	*cell = cellByPos( x, y );
			if (!cell)
			{
				out_predict_response = insertionOptions.KF_defaultCellMeanValue;
				out_predict_response_variance = square( insertionOptions.KF_initialCellStd ) + square(insertionOptions.KF_observationModelNoise);
			}
			else
			{
				out_predict_response = cell->kf_mean;
				out_predict_response_variance = square( cell->kf_std ) +  square(insertionOptions.KF_observationModelNoise);
			}
		}
		break;

	default:
		THROW_EXCEPTION("Invalid map type.");
	};

	// Un-do the sensor normalization:
	out_predict_response = insertionOptions.R_min + out_predict_response * ( insertionOptions.R_max - insertionOptions.R_min );

	MRPT_END
}


/*---------------------------------------------------------------
					insertObservation_KF2
  ---------------------------------------------------------------*/
void  CWirelessPowerGridMap2D::insertObservation_KF2(
	float			normReading,
	const CPose3D	&sensorPose_ )
{
	MRPT_START;

	const signed	W = insertionOptions.KF_W_size;
	const size_t	K = 2*W*(W+1)+1;
	const size_t	W21	= 2*W+1;
	const size_t	W21sqr	= W21*W21;

	ASSERT_(W>=2);

	m_hasToRecoverMeanAndCov = true;

	const TWirelessPowerCell defCell(
				insertionOptions.KF_defaultCellMeanValue,	// mean
				-1	// Just to indicate that cells are new, next changed to: insertionOptions.KF_initialCellStd			// std
				);

	const TPose3D sensorPose= TPose3D(sensorPose_);
	const TPose2D sensorPose2D = TPose2D( sensorPose );

	// Assure we have room enough in the grid!
	const double Aspace = (W+1) * m_resolution;

	resize(	sensorPose2D.x - Aspace,
			sensorPose2D.x + Aspace,
			sensorPose2D.y - Aspace,
			sensorPose2D.y + Aspace,
			defCell,
			Aspace );

	// --------------------------------------------------------
	// The Kalman-Filter estimation of the wifi grid-map:
	// --------------------------------------------------------
	const size_t	N = m_map.size();

	ASSERT_(K==m_stackedCov.getColCount());
	ASSERT_(N==m_stackedCov.getRowCount());

	// Prediction stage of KF:
	// ------------------------------------
	// Nothing to do here (static map)

	// Update stage of KF:
	//  We directly apply optimized formulas arising
	//   from our concrete sensor model.
	// -------------------------------------------------

	//const double	KF_covSigma2 = square(insertionOptions.KF_covSigma);
	//const double	std0 = insertionOptions.KF_initialCellStd;
	//const double	res2 = square(m_resolution);

	const int				cellIdx = xy2idx( sensorPose.x, sensorPose.y );
	TWirelessPowerCell	*cell = cellByPos( sensorPose.x, sensorPose.y );
	ASSERT_(cell!=NULL);

	// Predicted observation mean
	double		yk = normReading - cell->kf_mean;

	// Predicted observation variance
	double		sk =
		m_stackedCov(cellIdx,0) +	// Variance of that cell: cov(i,i)
		square(insertionOptions.KF_observationModelNoise);

	double		sk_1 = 1.0 / sk;

	std::vector<TWirelessPowerCell>::iterator	it;

#define WIFIGRIDMAP_KF2_VERBOSE		0

#if WIFIGRIDMAP_KF2_VERBOSE
	CTicTac		tictac;
	printf("[insertObservation_KF2] Updating mean values...");
	tictac.Tic();
#endif

	// ------------------------------------------------------------
	// Update mean values:
	//  Process only those cells in the vecinity of the cell (c):
	//
	//   What follows is *** REALLY UGLY *** for efficiency, sorry!!  :-)
	// ------------------------------------------------------------
	const int	cx_c = x2idx( sensorPose.x );
	const int	cy_c = y2idx( sensorPose.y );

	const int	Acx0 = max(-W, -cx_c);
	const int	Acy0 = max(-W, -cy_c);
	const int	Acx1 = min( W, int(m_size_x)-1-cx_c);
	const int	Acy1 = min( W, int(m_size_y)-1-cy_c);

	// We will fill this now, so we already have it for updating the
	//  covariances next:
	vector_double	cross_covs_c_i( W21sqr, 0); // Indexes are relative to the (2W+1)x(2W+1) window centered at "cellIdx".
	vector_int		window_idxs   ( W21sqr, -1 ); // The real-map indexes for each element in the window, or -1 if there are out of the map (for cells close to the border)

	// 1) First, the cells before "c":
	for (int Acy=Acy0;Acy<=0;Acy++)
	{
		int		limit_cx = Acy<0 ? Acx1 : -1;

		size_t  idx = cx_c+Acx0 + m_size_x * ( cy_c+Acy );
		int		idx_c_in_idx = -Acy*W21 - Acx0;

		int		window_idx = Acx0+W + (Acy+W)*W21;

		for (int Acx=Acx0;Acx<=limit_cx;Acx++)
		{
			ASSERT_(idx_c_in_idx>0);
			const double cov_i_c = m_stackedCov(idx,idx_c_in_idx);
			m_map[idx].kf_mean += yk * sk_1 * cov_i_c;

			// Save window indexes:
			cross_covs_c_i[window_idx] = cov_i_c;
			window_idxs[window_idx++]  = idx;

			idx_c_in_idx--;
			idx++;
		}
	}


	// 2) The cell "c" itself, and the rest within the window:
	for (int Acy=0;Acy<=Acy1;Acy++)
	{
		int start_cx = Acy>0 ? Acx0 : 0;

		size_t  idx = cx_c+start_cx + m_size_x * ( cy_c+Acy );
		int		idx_i_in_c;
		if (Acy>0)
				idx_i_in_c= (W+1) + (Acy-1)*W21+ (start_cx + W);	// Rest of rows
		else	idx_i_in_c= 0;	// First row.

		int		window_idx = start_cx+W + (Acy+W)*W21;

		for (int Acx=start_cx;Acx<=Acx1;Acx++)
		{
			ASSERT_(idx_i_in_c>=0 && idx_i_in_c<int(K));

			double cov_i_c = m_stackedCov(cellIdx,idx_i_in_c);
			m_map[idx].kf_mean += yk * sk_1 * cov_i_c;

			// Save window indexes:
			cross_covs_c_i[window_idx] = cov_i_c;
			window_idxs[window_idx++]  = idx;

			idx_i_in_c++;
			idx++;
		}
	}

	// ------------------------------------------------------------
	// Update cross-covariances values:
	//  Process only those cells in the vecinity of the cell (c)
	// ------------------------------------------------------------

	// First, we need the cross-covariances between each cell (i) and
	//  (c) in the window around (c). We have this in "cross_covs_c_i[k]"
	//  for k=[0,(2K+1)^2-1], and the indexes in "window_idxs[k]".
	for (size_t i=0;i<W21sqr;i++)
	{
		const int idx_i = window_idxs[i];
		if (idx_i<0) continue;	// out of the map

		// Extract the x,y indexes:
		int cx_i, cy_i;
		idx2cxcy(idx_i,cx_i,cy_i);

		const double cov_c_i = cross_covs_c_i[i];

		for (size_t j=i;j<W21sqr;j++)
		{
			const int idx_j = window_idxs[j];
			if (idx_j<0) continue;	// out of the map

			int cx_j, cy_j;
			idx2cxcy(idx_j,cx_j,cy_j);

			// The cells (i,j) may be too far from each other:
			const int Ax = cx_j-cx_i;
			if (Ax>W) continue;  // Next cell (may be more rows after the current one...)

			const int Ay = cy_j-cy_i;
			if (Ay>W) break; // We are absolutely sure out of i's window.

			const double cov_c_j = cross_covs_c_i[j];

			int idx_j_in_i;
			if (Ay>0)
					idx_j_in_i = Ax+W + (Ay-1)*W21 + W+1;
			else	idx_j_in_i = Ax; // First row:

			// Kalman update of the cross-covariances:
			double &cov_to_change = m_stackedCov(idx_i,idx_j_in_i);
			double Delta_cov = cov_c_j * cov_c_i * sk_1;
			if (i==j && cov_to_change<Delta_cov)
				THROW_EXCEPTION_CUSTOM_MSG1("Negative variance value appeared! Please increase the size of the window (W).\n(insertionOptions.KF_covSigma=%f)",insertionOptions.KF_covSigma);

			cov_to_change -= Delta_cov;

		} // end for j
	} // end for i


#if WIFIGRIDMAP_KF2_VERBOSE
	printf("Done in %.03fms\n",	tictac.Tac()*1000 );
#endif

	// Update covariance matrix values:
	// ---------------------------------------------------------


#if WIFIGRIDMAP_KF2_VERBOSE
	printf("[insertObservation_KF2] Updating covariance matrix...");
	tictac.Tic();
#endif



#if WIFIGRIDMAP_KF2_VERBOSE
	printf("Done! %.03fms\n",	tictac.Tac()*1000 );
#endif


	MRPT_END;
}
/*---------------------------------------------------------------
					recoverMeanAndCov
  ---------------------------------------------------------------*/
void  CWirelessPowerGridMap2D::recoverMeanAndCov() const
{
	if (!m_hasToRecoverMeanAndCov || (m_mapType!=mrKalmanApproximate ) ) return;
	m_hasToRecoverMeanAndCov = false;

	// Just recover the std of each cell:
	const size_t	N = m_map.size();
	for (size_t i=0;i<N;i++)
		m_map_castaway_const()[i].kf_std = sqrt( m_stackedCov(i,0) );
}


void CWirelessPowerGridMap2D::getMeanAndCov( vector_double &out_means, CMatrixDouble &out_cov) const
{
	const size_t N = BASE::m_map.size();
	out_means.resize(N);
	for (size_t i=0;i<N;++i)
		out_means[i] = BASE::m_map[i].kf_mean;

	recoverMeanAndCov();
	out_cov = m_cov;
}


/*---------------------------------------------------------------
				CWirelessPower_estimation
  ---------------------------------------------------------------*/
	// First order estimator model with non-linear dynamics compensation : x(i-N) =  ((y(i) -y(i-1))/(incT* k)) + y(i);
void CWirelessPowerGridMap2D::CWirelessPower_estimation (
	float	reading,
	const	CPose3D	&sensorPose,
	const	mrpt::system::TTimeStamp timestamp )
{
	int N;	//Memory efect delay

	// Check if estimation posible (at least one previous reading)
	if ( !m_lastObservations.empty() )
	{

		//Enose movement speed
		double speed_x = sensorPose.x() - m_new_Obs.sensorPose.x();
		double speed_y = sensorPose.y() - m_new_Obs.sensorPose.y();
		double incT = mrpt::system::timeDifference(m_new_Obs.timestamp,timestamp);

		if ( (incT >0) & (!first_incT) ){	//not the same sample (initialization of buffers)
			if (fixed_incT == 0)
				fixed_incT = incT;
			else
				ASSERT_(fabs(incT - fixed_incT) < (double)(0.05));
			m_new_Obs.speed = (sqrt (speed_x*speed_x + speed_y*speed_y)) / incT;
		}
		else
		{
			m_new_Obs.speed = 0;
			if (incT > 0)
				first_incT = false;
		}


		//slope>=0 -->Rise
		if ( reading >= m_new_Obs.reading )
		{
			m_new_Obs.k = 1.0/insertionOptions.tauR;
            N = 1;	//Memory effect compensation
		}
		//slope<0 -->decay
		else
		{
			//start decaying
			if (m_new_Obs.k == (float) (1.0/insertionOptions.tauR) ){
				//Use amplitude or just value?
				// Non-Linear compensation = f(sensor, amplitude, speed)
				m_new_Obs.k = 1.0/mrpt::math::leastSquareLinearFit((reading - insertionOptions.R_min),insertionOptions.calibrated_tauD_voltages,insertionOptions.calibrated_tauD_values,false);
			}else{
				//Do Nothing, keep the same tauD as last observation

			}// end-if(start decaying)

			//Dealy effect compensation
			N = mrpt::math::leastSquareLinearFit(m_new_Obs.speed ,insertionOptions.calibrated_delay_RobotSpeeds, insertionOptions.calibrated_delay_values,false);
			N = round(N);

			if (N >insertionOptions.lastObservations_size -1)
			{
				N = insertionOptions.lastObservations_size-1;
			}
		}//end-if  tau = f(slope)


		//New estimation values -- Ziegler-Nichols model --
		if( incT >0)
			//Initially there may come repetetive values till m_antiNoise_window is completed.
			m_new_Obs.estimation = ( ((reading -m_new_Obs.reading)/(incT* m_new_Obs.k)) )+ reading;
		else
			m_new_Obs.estimation = reading;


        //Prepare the New observation
		m_new_Obs.timestamp = timestamp ;
		m_new_Obs.reading = reading;
		m_new_Obs.sensorPose = sensorPose;

	}else{
		// First reading (use default values)
		m_new_Obs.k = 1.0/insertionOptions.tauR;
		m_new_Obs.reading = reading;
		m_new_Obs.timestamp = timestamp;
		m_new_Obs.speed = 0;
		m_new_Obs.sensorPose = sensorPose;
		m_new_Obs.estimation = reading;
		N = 1;

		//initialize the queue
		for (int i=0; i<insertionOptions.lastObservations_size; i++)
			m_lastObservations.push_back(m_new_Obs);

	}//end-if estimation values



	//Update m_lastObservations (due to memory efect)
	if (m_lastObservations[1].estimation == -20.0){
		//Copy right
		m_lastObservations[1].estimation = m_lastObservations[0].estimation;
	}

	m_lastObservations.erase( m_lastObservations.begin() );	//Erase the first element (the oldest)
	m_lastObservations.push_back(m_new_Obs);										//Add NULL obs as actual Obss
	m_lastObservations.rbegin()->estimation = -20.0;	//Non valid value to decect non valid observation

	//Modify queue estimation in the -Nth position
	try{
		m_lastObservations.at(insertionOptions.lastObservations_size - N-1).estimation = m_new_Obs.estimation;

	}catch(...){
		cout << "Error al acceder al array de m_readings \n" ;
		mrpt::system::pause();
	}

}//end-CWirelessPower_estimation



/*---------------------------------------------------------------
			noise_filtering
---------------------------------------------------------------*/
void CWirelessPowerGridMap2D::noise_filtering (
	float	reading,
	const	CPose3D	&sensorPose,
	const	mrpt::system::TTimeStamp timestamp )
{
	float partial_sum;

	m_new_ANS.reading = reading;
	m_new_ANS.timestamp = timestamp;
	m_new_ANS.sensorPose = sensorPose;

	if ( m_antiNoise_window.empty() )
	{
		// First reading (use default values)
		m_new_ANS.reading_filtered = reading;

		//initialize the queue
		m_antiNoise_window.assign( insertionOptions.winNoise_size, m_new_ANS );

	}else{
		m_antiNoise_window.erase( m_antiNoise_window.begin() );	//Erase the first element (the oldest)
		m_antiNoise_window.push_back(m_new_ANS);
	}

	//Average data to reduce noise
	partial_sum = 0;
	for (size_t i=0; i<m_antiNoise_window.size(); i++)
		partial_sum += m_antiNoise_window.at(i).reading;

	m_antiNoise_window.at(insertionOptions.winNoise_size/2).reading_filtered = partial_sum / insertionOptions.winNoise_size;

}