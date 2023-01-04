#include "Simulator.h"



void CALLBACK MyDispatchProc1(SIMCONNECT_RECV *pData, DWORD cbData, void *pContext) {
	switch (pData->dwID) {
		case SIMCONNECT_RECV_ID_SIMOBJECT_DATA: {
			SIMCONNECT_RECV_SIMOBJECT_DATA *pObjData = (SIMCONNECT_RECV_SIMOBJECT_DATA *) pData;

			switch (pObjData->dwRequestID) {
				case REQUEST_OWN_AIRCRAFT:
					DataOwnAircraft *pS = (DataOwnAircraft *) &pObjData->dwData;
					pThis->own                    = pS;
					qDebug() << pS->latitude << pS->longitude << pS->altitude;
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

Simulator::Simulator() {
	own = new DataOwnAircraft();
	pThis = this;
}

bool Simulator::initSimEvents() {
	HRESULT hr;
	if (SUCCEEDED(SimConnect_Open(&hSimConnect, "SkylineVoice", NULL, 0, NULL, 0))) {
		// DATA
		qDebug() << "\nSimConnect Connected!\n";
		initOwnAircraft(hSimConnect);
		// EVERY SECOND REQUEST DATA FOR DEFINITION 1 ON THE CURRENT USER AIRCRAFT (SIMCONNECT_OBJECT_ID_USER)
		hr += SimConnect_RequestDataOnSimObject(hSimConnect, REQUEST_OWN_AIRCRAFT, DEFINITION_OWN_AIRCRAFT,
			SIMCONNECT_OBJECT_ID_USER, SIMCONNECT_PERIOD_SIM_FRAME);
		timer = new QTimer();
		timer->setInterval(10);
		timer->start();
		connect(timer, &QTimer::timeout, this, &Simulator::onPosTimerElipsed);
		return true;
	} else {
		qDebug() << "\nFailed to Connect!!!!\n";
		return false;
	}
	return false;
}

void Simulator::onPosTimerElipsed() {
	callProc();
	qDebug() << own->com1ActiveMHz << own->com2ActiveMHz << own->comReceiveAll << "\n";
}

void Simulator::closeSimconnect() {
	SimConnect_Close(hSimConnect);
}

void Simulator::callProc() {
	SimConnect_CallDispatch(hSimConnect, MyDispatchProc1, NULL);
}

bool Simulator::initOwnAircraft(const HANDLE hSimConnect) {
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
