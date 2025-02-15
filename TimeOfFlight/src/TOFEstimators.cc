#include "TOFEstimators.h"
#include "TOFUtils.h"

#include <chrono>

#include "EVENT/LCCollection.h"
#include "UTIL/PIDHandler.h"

#include "marlin/Global.h"
#include "marlin/ProcessorEventSeeder.h"
#include "marlin/VerbosityLevels.h"
#include "marlinutil/GeometryUtil.h"
#include "MarlinTrk/Factory.h"
#include "EVENT/SimTrackerHit.h"
#include "UTIL/LCRelationNavigator.h"
#include "CLHEP/Random/Randomize.h"

using namespace TOFUtils;
using std::vector;
using std::string;
using EVENT::LCCollection;
using EVENT::ReconstructedParticle;
using EVENT::TrackerHit;
using EVENT::Track;
using EVENT::SimTrackerHit;
using EVENT::Cluster;
using EVENT::CalorimeterHit;
using EVENT::TrackState;
using EVENT::LCObject;
using UTIL::LCRelationNavigator;
using CLHEP::RandGauss;
using dd4hep::rec::Vector3D;

TOFEstimators aTOFEstimators ;


TOFEstimators::TOFEstimators() : marlin::Processor("TOFEstimators") {
    _description = "TOFEstimators processor computes momentum harmonic mean, track length \
                    and time-of-flight of the chosen ReconstructedParticle to the \
                    specified end point (SET hit or Ecal surface). To be used for a further particle ID";

    registerInputCollection(LCIO::RECONSTRUCTEDPARTICLE,
                            "ReconstructedParticleCollection",
                            "Name of the ReconstructedParticle collection",
                            _pfoCollectionName,
                            std::string("PandoraPFOs") );

    registerProcessorParameter("ExtrapolateToEcal",
                            "If true, track is extrapolated to the Ecal surface for track length calculation, \
                            time of flight estimated using Ecal hits. If false, track length calculated to the last tracker hit.\
                            Time of flight estimated using SET hit if exists.",
                            _extrapolateToEcal,
                            bool(true));

    registerProcessorParameter("TofMethod",
                            "name of the algorithm which estimates time of flight\
                            to the Ecal surface based on Ecal hits time information.\
                            Available options are: closest, frankAvg, frankFit.\
                            In case of _extrapolateToEcal==false is ignored",
                            _tofMethod,
                            std::string("closest") );

    registerProcessorParameter("TimeResolution",
                            "Time resolution of individual SET strips or Ecal hits in ps",
                            _timeResolution,
                            double(0.));

    registerProcessorParameter("MaxEcalLayer",
                            "Time of flight is calculated using Ecal hits only up to MaxLayer",
                            _maxEcalLayer,
                            int(10) );

}


void TOFEstimators::init(){

    if(_tofMethod != "closest" && _tofMethod != "frankAvg" && _tofMethod != "frankFit"){
        throw EVENT::Exception( "Invalid steering parameter for TofMethod is passed: " + _tofMethod + "\n Available options are: closest, frankAvg, frankFit" );
    }

    marlin::Global::EVENTSEEDER->registerProcessor(this);

    _outputParNames = {"momentumHM", "trackLength", "timeOfFlight"};
    _bField = MarlinUtil::getBzAtOrigin();
    _tpcOuterR = getTPCOuterR();
    // internally we use time resolution in nanoseconds
    _timeResolution = _timeResolution/1000.;

    _trkSystem = MarlinTrk::Factory::createMarlinTrkSystem("DDKalTest", nullptr, "");
    _trkSystem->setOption( MarlinTrk::IMarlinTrkSystem::CFG::useQMS, true);
    _trkSystem->setOption( MarlinTrk::IMarlinTrkSystem::CFG::usedEdx, true);
    _trkSystem->setOption( MarlinTrk::IMarlinTrkSystem::CFG::useSmoothing, true);
    _trkSystem->init();
}


void TOFEstimators::processEvent(EVENT::LCEvent * evt){
    RandGauss::setTheSeed( marlin::Global::EVENTSEEDER->getSeed(this) );
    ++_nEvent;
    streamlog_out(MESSAGE)<<"************Event************ "<<_nEvent<<std::endl;
    auto startTime = std::chrono::steady_clock::now();

    LCCollection* pfos = evt->getCollection(_pfoCollectionName);
    LCCollection* setRelations = evt->getCollection("SETSpacePointRelations");
    
    LCRelationNavigator navigatorSET = LCRelationNavigator( setRelations );

    PIDHandler pidHandler( pfos );
    int algoID = pidHandler.addAlgorithm( name(), _outputParNames );


    for (int i=0; i<pfos->getNumberOfElements(); ++i){
        ReconstructedParticle* pfo = static_cast <ReconstructedParticle*> ( pfos->getElementAt(i) );

        int nClusters = pfo->getClusters().size();
        int nTracks = pfo->getTracks().size();

        if( nClusters != 1 || nTracks != 1){
            // Analyze only simple pfos. Otherwise write dummy zeros
            vector<float> results{0., 0., 0.};
            pidHandler.setParticleID(pfo , 0, 0, 0., algoID, results);
            continue;
        }
        Track* track = pfo->getTracks()[0];
        Cluster* cluster = pfo->getClusters()[0];

        ///////////////////////////////////////////////////////////////
        // This part calculates track length and momentum harmonic mean
        ///////////////////////////////////////////////////////////////
        vector<Track*> subTracks = getSubTracks(track);
        vector<TrackStateImpl> trackStates = getTrackStatesPerHit(subTracks, _trkSystem, _extrapolateToEcal, _bField);

        double trackLength = 0.;
        double harmonicMom = 0.;
        int nTrackStates = trackStates.size();
        for( int j=1; j < nTrackStates; ++j ){
            //we check which track length formula to use
            double nTurns = getHelixNRevolutions( trackStates[j-1], trackStates[j] );
            double arcLength;
            // we cannot calculate arc length for more than pi revolution using delta phi. Use formula with only z
            if ( nTurns <= 0.5 ) arcLength = getHelixArcLength( trackStates[j-1], trackStates[j] );
            else arcLength = getHelixLengthAlongZ( trackStates[j-1], trackStates[j] );

            Vector3D mom = getHelixMomAtTrackState( trackStates[j-1], _bField );
            trackLength += arcLength;
            harmonicMom += arcLength/mom.r2();
        }
        harmonicMom = std::sqrt(trackLength/harmonicMom);

        //////////////////////////////////////
        // This part calculates Time of flight
        //////////////////////////////////////
        double timeOfFlight = 0.;
        if( _extrapolateToEcal ){
            if (_tofMethod == "closest"){
                timeOfFlight = getTofClosest(cluster, track, _timeResolution);
            }
            else if (_tofMethod == "frankAvg"){
                vector<CalorimeterHit*> frankHits = selectFrankEcalHits(cluster, track, _maxEcalLayer, _bField);
                timeOfFlight = getTofFrankAvg(frankHits, track, _timeResolution);
            }
            else if (_tofMethod == "frankFit"){
                vector<CalorimeterHit*> frankHits = selectFrankEcalHits(cluster, track, _maxEcalLayer, _bField);
                timeOfFlight = getTofFrankFit(frankHits, track, _timeResolution);
            }
        }
        else{
            //define tof as an average time between two SET strips
            //if no SET hits found, tof alreasy is 0, just skip
            TrackerHit* hitSET = getSETHit(track, _tpcOuterR);
            if ( hitSET != nullptr ){
                const vector<LCObject*>& simHitsSET = navigatorSET.getRelatedToObjects( hitSET );
                if ( simHitsSET.size() >= 2 ){
                    //It must be always 2, but just in case...
                    if (simHitsSET.size() > 2) streamlog_out(WARNING)<<"Found more than two SET strip hits! Writing TOF as an average of the first two elements in the array."<<std::endl;

                    SimTrackerHit* simHitSETFront = static_cast <SimTrackerHit*>( simHitsSET[0] );
                    SimTrackerHit* simHitSETBack = static_cast <SimTrackerHit*>( simHitsSET[1] );
                    double timeFront = RandGauss::shoot(simHitSETFront->getTime(), _timeResolution);
                    double timeBack = RandGauss::shoot(simHitSETBack->getTime(), _timeResolution);
                        timeOfFlight = (timeFront + timeBack)/2.;
                }
                else if (simHitsSET.size() == 1){
                    streamlog_out(WARNING)<<"Found only one SET strip hit! Writing TOF from a single strip."<<std::endl;
                    SimTrackerHit* simHitSET = static_cast <SimTrackerHit*>(simHitsSET[0]);
                    timeOfFlight = RandGauss::shoot(simHitSET->getTime(), _timeResolution);
                }
                else{
                    // this happens very rarily (0.1%). When >1 simHits associated with a single strip none simHits are written by the DDSpacePointBuilder.
                    streamlog_out(WARNING)<<"Found NO simHits associated with the found SET hit! Writing TOF as 0."<<std::endl;
                }
            }
        }
        vector<float> results{float(harmonicMom), float(trackLength), float(timeOfFlight)};
        pidHandler.setParticleID(pfo , 0, 0, 0., algoID, results);
        streamlog_out(DEBUG6)<<"*****PFO***** "<< i+1<<std::endl;
        streamlog_out(DEBUG6)<<"momentum: "<< float(harmonicMom)<<" Gev"<<std::endl;
        streamlog_out(DEBUG6)<<"track length: "<< float(trackLength)<<" mm"<<std::endl;
        streamlog_out(DEBUG6)<<"time-of-flight: "<< float(timeOfFlight)<<" ns"<<std::endl;
    }

    auto endTime = std::chrono::steady_clock::now();
    std::chrono::duration<double> duration = endTime - startTime;
    streamlog_out(DEBUG7)<<"Time spent (sec): "<<duration.count()<<std::endl;
}
