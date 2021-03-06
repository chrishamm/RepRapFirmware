/*
 * PolarKinematics.h
 *
 *  Created on: 13 Oct 2017
 *      Author: David
 */

#ifndef SRC_MOVEMENT_KINEMATICS_POLARKINEMATICS_H_
#define SRC_MOVEMENT_KINEMATICS_POLARKINEMATICS_H_

#include "Kinematics.h"

class PolarKinematics : public Kinematics
{
public:
	PolarKinematics();

	// Overridden base class functions. See Kinematics.h for descriptions.
	const char *GetName(bool forStatusReport) const override;
	bool Configure(unsigned int mCode, GCodeBuffer& gb, StringRef& reply, bool& error) override;
	bool CartesianToMotorSteps(const float machinePos[], const float stepsPerMm[], size_t numVisibleAxes, size_t numTotalAxes, int32_t motorPos[], bool isCoordinated) const override;
	void MotorStepsToCartesian(const int32_t motorPos[], const float stepsPerMm[], size_t numVisibleAxes, size_t numTotalAxes, float machinePos[]) const override;
	bool IsReachable(float x, float y, bool isCoordinated) const override;
	bool LimitPosition(float position[], size_t numAxes, AxesBitmap axesHomed, bool isCoordinated) const override;
	void GetAssumedInitialPosition(size_t numAxes, float positions[]) const override;
	const char* HomingButtonNames() const override { return "RTZUVWABC"; }
	HomingMode GetHomingMode() const override { return homeIndividualMotors; }
	AxesBitmap AxesAssumedHomed(AxesBitmap g92Axes) const override;
	const char* GetHomingFileName(AxesBitmap toBeHomed, AxesBitmap alreadyHomed, size_t numVisibleAxes, AxesBitmap& mustHomeFirst) const override;
	bool QueryTerminateHomingMove(size_t axis) const override;
	void OnHomingSwitchTriggered(size_t axis, bool highEnd, const float stepsPerMm[], DDA& dda) const override;
	void LimitSpeedAndAcceleration(DDA& dda, const float *normalisedDirectionVector) const override;

private:
	static constexpr float DefaultSegmentsPerSecond = 100.0;
	static constexpr float DefaultMinSegmentSize = 0.2;
	static constexpr float DefaultMaxRadius = 150.0;
	static constexpr float DefaultMaxTurntableSpeed = 30.0;				// degrees per second
	static constexpr float DefaultMaxTurntableAcceleration = 30.0;		// degrees per second per second
	static constexpr const char *HomeRadiusFileName = "homeradius.g";
	static constexpr const char *HomeBedFileName = "homebed.g";

	void Recalc();

	float minRadius, maxRadius, homedRadius;
	float maxTurntableSpeed, maxTurntableAcceleration;

	float minRadiusSquared, maxRadiusSquared;
};

#endif /* SRC_MOVEMENT_KINEMATICS_POLARKINEMATICS_H_ */
