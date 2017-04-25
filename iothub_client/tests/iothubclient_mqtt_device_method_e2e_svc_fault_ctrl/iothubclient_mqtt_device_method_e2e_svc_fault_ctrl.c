// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#include "iothubclient_common_device_method_e2e.h"
#include "testrunnerswitcher.h"
#include "iothubtransportmqtt.h"
#include "iothub_devicemethod.h"


static TEST_MUTEX_HANDLE g_dllByDll;

BEGIN_TEST_SUITE(iothubclient_mqtt_device_method_e2e_svc_fault_ctrl)

    TEST_SUITE_INITIALIZE(TestClassInitialize)
    {
        TEST_INITIALIZE_MEMORY_DEBUG(g_dllByDll);
        device_method_e2e_init();
    }

    TEST_SUITE_CLEANUP(TestClassCleanup)
    {
        device_method_e2e_deinit();
        TEST_DEINITIALIZE_MEMORY_DEBUG(g_dllByDll);
    }

    TEST_FUNCTION_CLEANUP(TestFunctionCleanup)
    {
        device_method_function_cleanup();
    }

    //// FAIL - don't recover
    //TEST_FUNCTION(IotHub_Mqtt_Method_Call_svc_fault_ctrl_kill_Tcp)
    //{
    //    device_method_e2e_method_call_svc_fault_ctrl_kill_Tcp(MQTT_Protocol, true);
    //}

    END_TEST_SUITE(iothubclient_mqtt_device_method_e2e_svc_fault_ctrl)

