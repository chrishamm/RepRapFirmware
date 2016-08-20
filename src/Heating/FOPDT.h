/*
 * FOPDT.h
 *
 *  Created on: 16 Aug 2016
 *      Author: David
 *
 *  Class to represent the parameters of a first order process with dead time
 */

#ifndef SRC_HEATING_FOPDT_H_
#define SRC_HEATING_FOPDT_H_

struct PidParams
{
	float kP;
	float kI;
	float kD;
};

class FopDt
{
public:
	bool SetParameters(float pg, float ptc, float pdt, float pMaxPwm, bool pUsePid);

	float GetGain() const { return gain; }
	float GetTimeConstant() const { return timeConstant; }
	float GetDeadTime() const { return deadTime; }
	float GetMaxPwm() const { return maxPwm; }
	bool UsePid() const { return usePid; }

	const PidParams& GetPidParameters(bool forLoadChange) const
	{
		return (forLoadChange) ? loadChangeParams : setpointChangeParams;
	}

private:
	void CalcPidConstants();

	float gain;
	float timeConstant;
	float deadTime;
	float maxPwm;
	bool usePid;

	PidParams setpointChangeParams;		// parameters for handling changes in the setpoint
	PidParams loadChangeParams;			// parameters for handling changes in the load
};

#endif /* SRC_HEATING_FOPDT_H_ */
