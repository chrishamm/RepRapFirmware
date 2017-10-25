/*
 * ScaraKinematics.h
 *
 *  Created on: 24 Apr 2017
 *      Author: David
 */

#ifndef SRC_MOVEMENT_KINEMATICS_SCARAKINEMATICS_H_
#define SRC_MOVEMENT_KINEMATICS_SCARAKINEMATICS_H_

#include "ZLeadscrewKinematics.h"

// Standard setup for SCARA machines assumed by this firmware
// The X motor output drives the proximal arm joint, unless remapped using M584
// The Y motor output drives the distal arm joint, unless remapped using M584
// Forward motion of a motor rotates the arm anticlockwise as seen from above (use M569 to reverse it)
// Theta is the angle of the proximal arm joint from the reference position.
// At theta = 0 the proximal arm points in the Cartesian +X direction
// Phi is the angle of the distal arm relative to the Cartesian X axis. Therefore the angle of the distal arm joint is (phi - theta).
// The M92 steps/mm settings for X and Y are interpreted as steps per degree of theta and phi respectively.

class ScaraKinematics : public ZLeadscrewKinematics
{
public:
	// Constructors
	ScaraKinematics();

	// Overridden base class functions. See Kinematics.h for descriptions.
	const char *GetName(bool forStatusReport) const override;
	bool Configure(unsigned int mCode, GCodeBuffer& gb, StringRef& reply, bool& error) override;
	bool CartesianToMotorSteps(const float machinePos[], const float stepsPerMm[], size_t numVisibleAxes, size_t numTotalAxes, int32_t motorPos[], bool allowModeChange) const override;
	void MotorStepsToCartesian(const int32_t motorPos[], const float stepsPerMm[], size_t numVisibleAxes, size_t numTotalAxes, float machinePos[]) const override;
	bool IsReachable(float x, float y) const override;
	bool LimitPosition(float position[], size_t numAxes, AxesBitmap axesHomed) const override;
	void GetAssumedInitialPosition(size_t numAxes, float positions[]) const override;
	size_t NumHomingButtons(size_t numVisibleAxes) const override;
	const char* HomingButtonNames() const override { return "PDZUVW"; }
	HomingMode GetHomingMode() const override { return homeIndividualMotors; }
	AxesBitmap AxesAssumedHomed(AxesBitmap g92Axes) const override;
	const char* GetHomingFileName(AxesBitmap toBeHomed, AxesBitmap alreadyHomed, size_t numVisibleAxes, AxesBitmap& mustHomeFirst) const override;
	bool QueryTerminateHomingMove(size_t axis) const override;
	void OnHomingSwitchTriggered(size_t axis, bool highEnd, const float stepsPerMm[], DDA& dda) const override;

private:
	static constexpr float DefaultSegmentsPerSecond = 100.0;
	static constexpr float DefaultMinSegmentSize = 0.2;
	static constexpr float DefaultProximalArmLength = 100.0;
	static constexpr float DefaultDistalArmLength = 100.0;
	static constexpr float DefaultMinTheta = -90.0;					// minimum proximal joint angle
	static constexpr float DefaultMaxTheta = 90.0;					// maximum proximal joint angle
	static constexpr float DefaultMinPhi = -135.0;					// minimum distal joint angle
	static constexpr float DefaultMaxPhi = 135.0;					// maximum distal joint angle

	static constexpr const char *HomeProximalFileName = "homeproximal.g";
	static constexpr const char *HomeDistalFileName = "homedistal.g";

	void Recalc();

	// Primary parameters
	float proximalArmLength;
	float distalArmLength;
	float thetaLimits[2];							// minimum proximal joint angle
	float phiLimits[2];								// minimum distal joint angle
	float crosstalk[3];								// if we rotate the distal arm motor, for each full rotation the Z height goes up by this amount
	float xOffset;									// where bed X=0 is relative to the proximal joint
	float yOffset;									// where bed Y=0 is relative to the proximal joint

	// Derived parameters
	float minRadius, minRadiusSquared;
	float maxRadius, maxRadiusSquared;
	float proximalArmLengthSquared;
	float distalArmLengthSquared;
	float twoPd;

	// State variables
	mutable bool isDefaultArmMode;					// this should be moved into class Move when it knows about different arm modes
};

#endif /* SRC_MOVEMENT_KINEMATICS_SCARAKINEMATICS_H_ */
