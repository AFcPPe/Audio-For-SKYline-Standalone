#ifndef SIMULATOR_H
#define SIMULATOR_H
#include "SimConnect/include/SimConnect.h"
#include <QtCore/QObject>
#include <QtCore/qtimer.h>
#include <QtCore/qdebug.h>
#include "Global.h"
#include "XPC/xplaneConnect.h"
enum DATA_DEFINE_ID {
	DEFINITION_OWN_AIRCRAFT,
};

enum DATA_REQUEST_ID {
	REQUEST_OWN_AIRCRAFT,
};

struct DataOwnAircraft {
	double latitude;
	double longitude;
	double altitude;
	double onGround;
	double com1ActiveMHz = 118.0;  //!< COM1 active frequency
	double com2ActiveMHz = 118.0; //!< COM2 active frequency
	double com1StandbyMHz; //!< COM1 standby frequency
	double com2StandbyMHz; //!< COM2 standby frequency
	double comTransmit1;   //!< COM1 transmit, means also receiving
	double comTransmit2;   //!< COM2 transmit, means also receiving
	double comReceiveAll;  //!< all COMs receiving, or COM:x transmitting or receiving
	double comTest1;       //!< COM1 test
	double comTest2;       //!< COM2 test
	double comStatus1;     //!< COM1 status
	double comStatus2;     //!< COM2 status

};



class SimulatorSimConnect : public QObject {

Q_OBJECT

public:
	SimulatorSimConnect();

	bool initSimEvents();
	void closeSimconnect();
	void callProc();
	void getFromXplane();
	DataOwnAircraft *own;
	QTimer *timer;
	QTimer *connectTimer;
	int packetCount = 0;
	const char *XPCIP  = "127.0.0.1"; // IP Address of computer running X-Plane
	XPCSocket XPCsock;
	int mode = 0;

signals:
	void RaiseSimconnectConnected();
	void RaiseSimdataUpdated();
private slots:
	void onPosTimerElipsed();
	void onConnectTimerElipsed();

private:
	
	bool initOwnAircraft(const HANDLE hSimConnect);
	HANDLE hSimConnect = NULL;



};

static SimulatorSimConnect *pThis = NULL;

#endif