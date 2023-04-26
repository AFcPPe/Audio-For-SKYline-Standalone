#include "Simulator.h"
#include "MainWindow.h"



void CALLBACK MyDispatchProc1(SIMCONNECT_RECV *pData, DWORD cbData, void *pContext) {
	switch (pData->dwID) {
		case SIMCONNECT_RECV_ID_SIMOBJECT_DATA: {
			SIMCONNECT_RECV_SIMOBJECT_DATA *pObjData = (SIMCONNECT_RECV_SIMOBJECT_DATA *) pData;

			switch (pObjData->dwRequestID) {
				case REQUEST_OWN_AIRCRAFT:
					DataOwnAircraft *pS = (DataOwnAircraft *) &pObjData->dwData;
					pThis->packetCount  = 0;
					pThis->own                    = pS;
					emit pThis->RaiseSimdataUpdated();
					//qDebug()<<"\n======================\n" << pS->com1ActiveMHz << pS->com2ActiveMHz << pS->comReceiveAll << "\n======================\n";
					break;
			}
			break;
		}

		case SIMCONNECT_RECV_ID_QUIT: {
			//        pThis->quit = 1;
			break;
		}

		default:
			break;
	}
}

SimulatorSimConnect::SimulatorSimConnect() {
	own = new DataOwnAircraft();
	pThis = this;
	connectTimer = new QTimer();
	connectTimer->setInterval(5000);
	connectTimer->start();
	connect(connectTimer, &QTimer::timeout, this, &SimulatorSimConnect::onConnectTimerElipsed);
	timer = new QTimer();
	timer->setInterval(1000);
	timer->start();
	connect(timer, &QTimer::timeout, this, &SimulatorSimConnect::onPosTimerElipsed);
}

void SimulatorSimConnect::onConnectTimerElipsed() {
	if (mode != 0)
	{
		return;
	}
	qDebug() << "========================\nTrying XPlane\n=============================";
	XPCsock = openUDP(XPCIP);
	float tVal[1];
	int tSize = 1;
	if (!(getDREF(XPCsock, "sim/test/test_float", tVal, &tSize) < 0)) {
		qDebug() << "========================\nGOOD FOR XPLANE!!!!!!!!!!!!!\n=============================";
		mode = 1;
		return;
	}
	qDebug() << "========================\nTrying MSFS2020\n=============================";
	if (this->initSimEvents()) {
		qDebug() << "========================\nGOOD FOR MSFS20220!!!!!!!!!!!!!\n=============================";
		mode = 2;
		return;
	}
}

bool SimulatorSimConnect::initSimEvents() {
	HRESULT hr;
	if (SUCCEEDED(SimConnect_Open(&hSimConnect, "SkylineVoice", NULL, 0, NULL, 0))) {
		// DATA
		qDebug() << "\nSimConnect Connected!\n";
		initOwnAircraft(hSimConnect);
		// EVERY SECOND REQUEST DATA FOR DEFINITION 1 ON THE CURRENT USER AIRCRAFT (SIMCONNECT_OBJECT_ID_USER)
		hr += SimConnect_RequestDataOnSimObject(hSimConnect, REQUEST_OWN_AIRCRAFT, DEFINITION_OWN_AIRCRAFT,
			SIMCONNECT_OBJECT_ID_USER, SIMCONNECT_PERIOD_SIM_FRAME);
		return true;
	} else {
		qDebug() << "\nFailed to Connect!!!!\n";
		return false;
	}
	return false;
}

void SimulatorSimConnect::getFromXplane() {
	const char *drefs[4] = { "sim/cockpit/radios/com1_freq_hz", // indicated airspeed
							 "sim/cockpit/radios/com2_freq_hz", // fuel quantity in each tank, a float array of size 10
							 "sim/cockpit/radios/com1_stdby_freq_hz", // pitch reading displayed on attitude indicator
							 "sim/cockpit/radios/com2_stdby_freq_hz" };
	float *values[4];
	unsigned char count = 4;
	values[0] = (float *) malloc(1 * sizeof(float));
	values[1] = (float *) malloc(
		1 * sizeof(float));
	values[2] = (float *) malloc(1 * sizeof(float));
	values[3] = (float *) malloc(1 * sizeof(float));

	int sizes[4] = { 1, 1, 1, 1 };
	if (getDREFs(XPCsock, drefs, values, count, sizes) < 0) {
		qDebug() << " XPC::An error occured.\n";
	} else {
		qDebug() << "XPC: Successfully got freqData\n";
		int com1a = (int) *values[0]*10;
		int com2a = (int) *values[1]*10;
		int com1s = (int) *values[2]*10;
		int com2s = (int) *values[3] * 10;
		com1a     = (com1a / 25 * 25 + ((com1a % 25 == 0) ? 0 : 25));
		com2a     = (com2a / 25 * 25 + ((com2a % 25 == 0) ? 0 : 25));
		com1s     = (com1s / 25 * 25 + ((com1s % 25 == 0) ? 0 : 25));
		com2s     = (com2s / 25 * 25 + ((com2s % 25 == 0) ? 0 : 25));
		own->com1ActiveMHz = com1a/1000.0;
		own->com2ActiveMHz  = com2a / 1000.0;
		own->com1StandbyMHz = com1s / 1000.0;
		own->com2StandbyMHz = com2s / 1000.0;
		packetCount = 0;
		emit RaiseSimdataUpdated();
	}


	delete values[0], values[1], values[2], values[3];


}

void SimulatorSimConnect::onPosTimerElipsed() {
	if (mode == 0) {
		return;
	}
	if (mode == 1)
	{
		getFromXplane();
	}else if (mode == 2){
		callProc();
	}
	
	packetCount++;
	if (packetCount >= 10) {
		mode = 0;
		qDebug() << "\n========================\nLost\n=============================\n";
		closeSimconnect();
		connectTimer->start();
	}
}

void SimulatorSimConnect::closeSimconnect() {
	SimConnect_Close(hSimConnect);
}

void SimulatorSimConnect::callProc() {
	SimConnect_CallDispatch(hSimConnect, MyDispatchProc1, NULL);
}

bool SimulatorSimConnect::initOwnAircraft(const HANDLE hSimConnect) {
	HRESULT hr = S_OK;
	hr += SimConnect_AddToDataDefinition(hSimConnect, DEFINITION_OWN_AIRCRAFT, "PLANE LATITUDE", "Degrees");
	hr += SimConnect_AddToDataDefinition(hSimConnect, DEFINITION_OWN_AIRCRAFT, "PLANE LONGITUDE", "Degrees");
	hr += SimConnect_AddToDataDefinition(hSimConnect, DEFINITION_OWN_AIRCRAFT, "PLANE ALT ABOVE GROUND", "Feet");
	hr += SimConnect_AddToDataDefinition(hSimConnect, DEFINITION_OWN_AIRCRAFT, "SIM ON GROUND", "Bool");
	hr += SimConnect_AddToDataDefinition(hSimConnect, DEFINITION_OWN_AIRCRAFT, "COM ACTIVE FREQUENCY:1", "MHz");
	hr += SimConnect_AddToDataDefinition(hSimConnect, DEFINITION_OWN_AIRCRAFT, "COM ACTIVE FREQUENCY:2", "MHz");
	hr += SimConnect_AddToDataDefinition(hSimConnect, DEFINITION_OWN_AIRCRAFT, "COM STANDBY FREQUENCY:1", "MHz");
	hr += SimConnect_AddToDataDefinition(hSimConnect, DEFINITION_OWN_AIRCRAFT, "COM STANDBY FREQUENCY:2", "MHz");
	hr += SimConnect_AddToDataDefinition(hSimConnect, DEFINITION_OWN_AIRCRAFT, "COM TRANSMIT:1", "Bool");
	hr += SimConnect_AddToDataDefinition(hSimConnect, DEFINITION_OWN_AIRCRAFT, "COM TRANSMIT:2", "Bool");
	hr += SimConnect_AddToDataDefinition(hSimConnect, DEFINITION_OWN_AIRCRAFT, "COM RECEIVE ALL", "Bool");
	hr += SimConnect_AddToDataDefinition(hSimConnect, DEFINITION_OWN_AIRCRAFT, "COM TEST:1", "Bool");
	hr += SimConnect_AddToDataDefinition(hSimConnect, DEFINITION_OWN_AIRCRAFT, "COM TEST:2", "Bool");
	hr += SimConnect_AddToDataDefinition(hSimConnect, DEFINITION_OWN_AIRCRAFT, "COM STATUS:1", "Enum");
	hr += SimConnect_AddToDataDefinition(hSimConnect, DEFINITION_OWN_AIRCRAFT, "COM STATUS:2", "Enum");
	return hr == S_OK;
}
