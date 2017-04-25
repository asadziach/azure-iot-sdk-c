// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

#ifdef __cplusplus
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <ctime>
#else
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#endif


#include "iothub_messaging_ll.h"

#include "umock_c.h"
#include "testrunnerswitcher.h"

#include "iothub_account.h"
#include "iothubtest.h"

#include "iothub_client_options.h"

#include "azure_c_shared_utility/lock.h"

#include "azure_c_shared_utility/platform.h"
#include "azure_c_shared_utility/threadapi.h"
#include "iothub_service_client_auth.h"
#include "iothub_devicemethod.h"
#include "iothub_client.h"

#include "iothubclient_common_device_method_e2e.h"

TEST_DEFINE_ENUM_TYPE(IOTHUB_CLIENT_RESULT, IOTHUB_CLIENT_RESULT_VALUES);
TEST_DEFINE_ENUM_TYPE(IOTHUB_DEVICE_METHOD_RESULT, IOTHUB_DEVICE_METHOD_RESULT_VALUES);
TEST_DEFINE_ENUM_TYPE(IOTHUB_CLIENT_CONNECTION_STATUS, IOTHUB_CLIENT_CONNECTION_STATUS_VALUES);

typedef struct IOTHUB_CONN_INFO_TAG
{
    IOTHUB_CLIENT_CONNECTION_STATUS conn_status;
    bool upload_status;
    LOCK_HANDLE lock;
} IOTHUB_CONN_INFO;

static IOTHUB_ACCOUNT_INFO_HANDLE g_iothubAcctInfo = NULL;
static IOTHUB_CONN_INFO g_conn_info;

static IOTHUB_CLIENT_HANDLE iotHubClientHandle = NULL;
static IOTHUB_SERVICE_CLIENT_AUTH_HANDLE iotHubServiceClientHandle = NULL;
static IOTHUB_SERVICE_CLIENT_DEVICE_METHOD_HANDLE serviceClientDeviceMethodHandle = NULL;


void device_method_e2e_init(void)
{
    ASSERT_ARE_EQUAL(int, 0, platform_init() );

    g_iothubAcctInfo = IoTHubAccount_Init();
    ASSERT_IS_NOT_NULL(g_iothubAcctInfo);

    // Initialize locking
    g_conn_info.lock = Lock_Init();
    g_conn_info.upload_status = false;
    ASSERT_IS_NOT_NULL(g_conn_info.lock);
}

void device_method_e2e_deinit(void)
{
    IoTHubAccount_deinit(g_iothubAcctInfo);

    // Need a double deinit
    platform_deinit();
    platform_deinit();

    Lock_Deinit(g_conn_info.lock);
}

static const char *METHOD_NAME = "MethodName";
static const int METHOD_RESPONSE_SUCCESS = 200;
static const int METHOD_RESPONSE_ERROR = 401;
static const unsigned int TIMEOUT = 60;
static const unsigned int IOTHUB_CONNECT_TIMEOUT_SEC = 30;

static void connection_status_callback(IOTHUB_CLIENT_CONNECTION_STATUS result, IOTHUB_CLIENT_CONNECTION_STATUS_REASON reason, void* userContextCallback)
{
    if (reason == IOTHUB_CLIENT_CONNECTION_OK)
    {
        IOTHUB_CONN_INFO* conn_info = (IOTHUB_CONN_INFO*)userContextCallback;
        if (Lock(conn_info->lock) != LOCK_OK)
        {
            ASSERT_FAIL("unable to lock");
        }
        else
        {
            conn_info->conn_status = result;
            (void)Unlock(conn_info->lock);
        }
    }
}

static int DeviceMethodCallback(const char* method_name, const unsigned char* payload, size_t size, unsigned char** response, size_t* resp_size, void* userContextCallback)
{
    int responseCode;
    const char * expectedMethodPayload = (const char*)userContextCallback;

    if (strcmp(METHOD_NAME, method_name))
    {
        LogError("Method name incorrect - expected %s but got %s", METHOD_NAME, method_name);
        responseCode = METHOD_RESPONSE_ERROR;
    }
    else if (size != strlen(expectedMethodPayload))
    {
        LogError("payload size incorect - expected %zu but got %zu", strlen(expectedMethodPayload), size);
        responseCode = METHOD_RESPONSE_ERROR;
    }
    else if (memcmp(payload, expectedMethodPayload, size))
    {
        LogError("Payload strings do not match");
        responseCode = METHOD_RESPONSE_ERROR;
    }
    else
    {
        *resp_size = size;
        if (size == 0)
        {
            *response = NULL;
            responseCode = METHOD_RESPONSE_SUCCESS;
        }
        else
        {
            if ((*response = (unsigned char*)malloc(*resp_size)) == NULL)
            {
                LogError("allocation failure");
                responseCode = METHOD_RESPONSE_ERROR;
            }
            else
            {
                (void)memcpy(*response, payload, *resp_size);
                responseCode = METHOD_RESPONSE_SUCCESS;
            }
        }
    }
    return responseCode;
}

DEFINE_ENUM_STRINGS(IOTHUB_CLIENT_FILE_UPLOAD_RESULT, IOTHUB_CLIENT_FILE_UPLOAD_RESULT_VALUES)
void fileUploadCallback(IOTHUB_CLIENT_FILE_UPLOAD_RESULT result, void* userContextCallback)
{
    LogInfo("fileUploadCallback(%s)", ENUM_TO_STRING(IOTHUB_CLIENT_FILE_UPLOAD_RESULT, result));

    IOTHUB_CONN_INFO* conn_info = (IOTHUB_CONN_INFO*)userContextCallback;
    if (Lock(conn_info->lock) != LOCK_OK)
    {
        ASSERT_FAIL("unable to lock");
    }
    else
    {
        conn_info->upload_status = result == FILE_UPLOAD_OK;
        (void)Unlock(conn_info->lock);
    }
}

#define HELLO_WORLD "Hello World from IoTHubClient_UploadToBlob"
static int DeviceMethodWithUploadCallback(const char* method_name, const unsigned char* payload, size_t size, unsigned char** response, size_t* resp_size, void* userContextCallback)
{
    (void)method_name;
    (void)payload;
    (void)size;
    (void)userContextCallback;

    int responseCode;
    if (IoTHubClient_UploadToBlobAsync(iotHubClientHandle, "hello_world.txt", (const unsigned char*)HELLO_WORLD, sizeof(HELLO_WORLD) - 1, fileUploadCallback, &g_conn_info) == IOTHUB_CLIENT_OK)
    {
        LogInfo("Upload succeeded");
        char* RESPONSE_STRING = "{ \"Response\": \"Nothing\" }";
        *resp_size = strlen(RESPONSE_STRING);
        if ((*response = (unsigned char*)malloc(*resp_size)) == NULL)
        {
            responseCode = METHOD_RESPONSE_ERROR;
        }
        else
        {
            (void)memcpy(*response, RESPONSE_STRING, *resp_size);
            responseCode = METHOD_RESPONSE_SUCCESS;
        }
    }
    else
    {
        LogError("Upload failed");
        responseCode = METHOD_RESPONSE_ERROR;
    }
    return responseCode;
}

void test_device_method_with_string(IOTHUB_PROVISIONED_DEVICE* deviceToUse, IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol, const char *payload)
{
    IOTHUB_CLIENT_RESULT result;
    time_t beginOperation, nowTime;
    bool trace = true;
    
    g_conn_info.conn_status = IOTHUB_CLIENT_CONNECTION_UNAUTHENTICATED;

    iotHubClientHandle = IoTHubClient_CreateFromConnectionString(deviceToUse->connectionString, protocol);
    ASSERT_IS_NOT_NULL_WITH_MSG(iotHubClientHandle, "Could not invoke IoTHubClient_CreateFromConnectionString");

    if (deviceToUse->howToCreate == IOTHUB_ACCOUNT_AUTH_X509)
    {
        result = IoTHubClient_SetOption(iotHubClientHandle, OPTION_X509_CERT, deviceToUse->certificate);
        ASSERT_ARE_EQUAL_WITH_MSG(IOTHUB_CLIENT_RESULT, IOTHUB_CLIENT_OK, result, "Could not set the device x509 certificate");
        result = IoTHubClient_SetOption(iotHubClientHandle, OPTION_X509_PRIVATE_KEY, deviceToUse->primaryAuthentication);
        ASSERT_ARE_EQUAL_WITH_MSG(IOTHUB_CLIENT_RESULT, IOTHUB_CLIENT_OK, result, "Could not set the device x509 privateKey");
    }

    result = IoTHubClient_SetConnectionStatusCallback(iotHubClientHandle, connection_status_callback, &g_conn_info);
    ASSERT_ARE_EQUAL_WITH_MSG(IOTHUB_CLIENT_RESULT, IOTHUB_CLIENT_OK, result, "Could not set connection Status Callback");

    // Turn on Log 
    (void)IoTHubClient_SetOption(iotHubClientHandle, OPTION_LOG_TRACE, &trace);

    result = IoTHubClient_SetDeviceMethodCallback(iotHubClientHandle, DeviceMethodCallback, (void*)payload);
    ASSERT_ARE_EQUAL_WITH_MSG(IOTHUB_CLIENT_RESULT, IOTHUB_CLIENT_OK, result, "Could not set the device method callback");

    bool continue_running = true;
    beginOperation = time(NULL);
    do
    {
        if (Lock(g_conn_info.lock) != LOCK_OK)
        {
            ASSERT_FAIL("unable to lock");
        }
        else
        {
            if (g_conn_info.conn_status == IOTHUB_CLIENT_CONNECTION_AUTHENTICATED)
            {
                continue_running = false;
            }
            (void)Unlock(g_conn_info.lock);
        }

        ThreadAPI_Sleep(10);
    } while (
        (nowTime = time(NULL)),
        (difftime(nowTime, beginOperation) < IOTHUB_CONNECT_TIMEOUT_SEC) &&
        (continue_running)
      );
    ASSERT_ARE_EQUAL_WITH_MSG(IOTHUB_CLIENT_CONNECTION_STATUS, IOTHUB_CLIENT_CONNECTION_AUTHENTICATED, g_conn_info.conn_status, "Device Not connected");

    // Wait for the method to subscribe
    ThreadAPI_Sleep(1 * 1000);
    
    iotHubServiceClientHandle = IoTHubServiceClientAuth_CreateFromConnectionString(IoTHubAccount_GetIoTHubConnString(g_iothubAcctInfo));
    ASSERT_IS_NOT_NULL_WITH_MSG(iotHubServiceClientHandle, "Could not create service client handle");
    
    serviceClientDeviceMethodHandle = IoTHubDeviceMethod_Create(iotHubServiceClientHandle);
    ASSERT_IS_NOT_NULL_WITH_MSG(serviceClientDeviceMethodHandle, "Could not create device method handle");

    int responseStatus;
    unsigned char* responsePayload;
    size_t responsePayloadSize;
    IOTHUB_DEVICE_METHOD_RESULT invokeResult = IoTHubDeviceMethod_Invoke(serviceClientDeviceMethodHandle, deviceToUse->deviceId, METHOD_NAME, payload, TIMEOUT, &responseStatus, &responsePayload, &responsePayloadSize);

    ASSERT_ARE_EQUAL_WITH_MSG(IOTHUB_DEVICE_METHOD_RESULT, IOTHUB_DEVICE_METHOD_OK, invokeResult, "Service Client IoTHubDeviceMethod_Invoke failed");
    ASSERT_ARE_EQUAL_WITH_MSG(int, METHOD_RESPONSE_SUCCESS, responseStatus, "response status is incorrect");

    ASSERT_ARE_EQUAL_WITH_MSG(size_t, strlen(payload), responsePayloadSize, "response size is incorrect");
    if (memcmp(payload, responsePayload, responsePayloadSize))
    {
        ASSERT_FAIL("response string does not match");
    }

    free(responsePayload);
}

void test_device_method_calls_upload(IOTHUB_PROVISIONED_DEVICE* deviceToUse, IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol, const char *payload)
{
    g_conn_info.conn_status = IOTHUB_CLIENT_CONNECTION_UNAUTHENTICATED;

    iotHubClientHandle = IoTHubClient_CreateFromConnectionString(deviceToUse->connectionString, protocol);
    ASSERT_IS_NOT_NULL_WITH_MSG(iotHubClientHandle, "Could not invoke IoTHubClient_CreateFromConnectionString");

    IOTHUB_CLIENT_RESULT result;
    if (deviceToUse->howToCreate == IOTHUB_ACCOUNT_AUTH_X509)
    {
        result = IoTHubClient_SetOption(iotHubClientHandle, OPTION_X509_CERT, deviceToUse->certificate);
        ASSERT_ARE_EQUAL_WITH_MSG(IOTHUB_CLIENT_RESULT, IOTHUB_CLIENT_OK, result, "Could not set the device x509 certificate");
        result = IoTHubClient_SetOption(iotHubClientHandle, OPTION_X509_PRIVATE_KEY, deviceToUse->primaryAuthentication);
        ASSERT_ARE_EQUAL_WITH_MSG(IOTHUB_CLIENT_RESULT, IOTHUB_CLIENT_OK, result, "Could not set the device x509 privateKey");
    }

    result = IoTHubClient_SetConnectionStatusCallback(iotHubClientHandle, connection_status_callback, &g_conn_info);
    ASSERT_ARE_EQUAL_WITH_MSG(IOTHUB_CLIENT_RESULT, IOTHUB_CLIENT_OK, result, "Could not set connection Status Callback");

    // Turn on Log 
    bool trace = true;
    (void)IoTHubClient_SetOption(iotHubClientHandle, OPTION_LOG_TRACE, &trace);

    bool continue_running = true;
    time_t nowTime;
    time_t beginOperation = time(NULL);
    do
    {
        if (Lock(g_conn_info.lock) != LOCK_OK)
        {
            ASSERT_FAIL("unable to lock");
        }
        else
        {
            if (g_conn_info.conn_status == IOTHUB_CLIENT_CONNECTION_AUTHENTICATED)
            {
                continue_running = false;
            }
            (void)Unlock(g_conn_info.lock);
        }

        ThreadAPI_Sleep(10);
    } while (
        (nowTime = time(NULL)),
        (difftime(nowTime, beginOperation) < IOTHUB_CONNECT_TIMEOUT_SEC) &&
        (continue_running)
        );
    ASSERT_ARE_EQUAL_WITH_MSG(IOTHUB_CLIENT_CONNECTION_STATUS, IOTHUB_CLIENT_CONNECTION_AUTHENTICATED, g_conn_info.conn_status, "Device Not connected");

    result = IoTHubClient_SetDeviceMethodCallback(iotHubClientHandle, DeviceMethodWithUploadCallback, (void*)payload);
    ASSERT_ARE_EQUAL_WITH_MSG(IOTHUB_CLIENT_RESULT, IOTHUB_CLIENT_OK, result, "Could not set the device method callback");

    // Wait for the method to subscribe
    ThreadAPI_Sleep(1 * 1000);

    iotHubServiceClientHandle = IoTHubServiceClientAuth_CreateFromConnectionString(IoTHubAccount_GetIoTHubConnString(g_iothubAcctInfo));
    ASSERT_IS_NOT_NULL_WITH_MSG(iotHubServiceClientHandle, "Could not create service client handle");

    serviceClientDeviceMethodHandle = IoTHubDeviceMethod_Create(iotHubServiceClientHandle);
    ASSERT_IS_NOT_NULL_WITH_MSG(serviceClientDeviceMethodHandle, "Could not create device method handle");

    int responseStatus;
    unsigned char* responsePayload = NULL;
    size_t responsePayloadSize = 0;
    IOTHUB_DEVICE_METHOD_RESULT invokeResult = IoTHubDeviceMethod_Invoke(serviceClientDeviceMethodHandle, deviceToUse->deviceId, METHOD_NAME, payload, TIMEOUT, &responseStatus, &responsePayload, &responsePayloadSize);

    // yield
    ThreadAPI_Sleep(1 * 1000);

    continue_running = true;
    beginOperation = time(NULL);
    do
    {
        if (Lock(g_conn_info.lock) != LOCK_OK)
        {
            ASSERT_FAIL("unable to lock");
        }
        else
        {
            continue_running = g_conn_info.upload_status != true;
            (void)Unlock(g_conn_info.lock);
        }

        ThreadAPI_Sleep(10);
    } while (
        (nowTime = time(NULL)),
        (difftime(nowTime, beginOperation) < IOTHUB_CONNECT_TIMEOUT_SEC) &&
        (continue_running)
        );

    free(responsePayload);

    ASSERT_ARE_EQUAL_WITH_MSG(IOTHUB_DEVICE_METHOD_RESULT, IOTHUB_DEVICE_METHOD_OK, invokeResult, "Service Client IoTHubDeviceMethod_Invoke failed");
    ASSERT_ARE_EQUAL_WITH_MSG(int, METHOD_RESPONSE_SUCCESS, responseStatus, "response status is incorrect");
}

static void client_create_with_properies_and_send_d2c(MAP_HANDLE mapHandle)
{
    IOTHUB_MESSAGE_HANDLE msgHandle;
    IOTHUB_CLIENT_RESULT result;

    const char* messageStr = "Happy little message";
    msgHandle = IoTHubMessage_CreateFromByteArray((const unsigned char*)messageStr, strlen(messageStr));
    ASSERT_IS_NOT_NULL_WITH_MSG(msgHandle, "Could not create the D2C message to be sent");

    MAP_HANDLE msgMapHandle = IoTHubMessage_Properties(msgHandle);

    const char*const* keys;
    const char*const* values;
    size_t propCount;

    MAP_RESULT mapResult = Map_GetInternals(mapHandle, &keys, &values, &propCount);
    if (mapResult == MAP_OK)
    {
        for (size_t i = 0; i < propCount; i++)
        {
            if (Map_AddOrUpdate(msgMapHandle, keys[i], values[i]) != MAP_OK)
            {
                ASSERT_FAIL("Map_AddOrUpdate failed!");
            }
        }
    }

    // act
    result = IoTHubClient_SendEventAsync(iotHubClientHandle, msgHandle, NULL, NULL);
    ASSERT_ARE_EQUAL_WITH_MSG(IOTHUB_CLIENT_RESULT, IOTHUB_CLIENT_OK, result, "SendEventAsync failed");
}

static void test_device_method_with_string_svc_fault_ctrl(IOTHUB_PROVISIONED_DEVICE* deviceToUse, IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol, const char *payload, const char* faultOperationType, const char* faultOperationCloseReason, const char* faultOperationDelayInSecs)
{
    IOTHUB_CLIENT_RESULT result;
    time_t beginOperation, nowTime;
    bool trace = true;

    g_conn_info.conn_status = IOTHUB_CLIENT_CONNECTION_UNAUTHENTICATED;

    iotHubClientHandle = IoTHubClient_CreateFromConnectionString(deviceToUse->connectionString, protocol);
    ASSERT_IS_NOT_NULL_WITH_MSG(iotHubClientHandle, "Could not invoke IoTHubClient_CreateFromConnectionString");

    if (deviceToUse->howToCreate == IOTHUB_ACCOUNT_AUTH_X509)
    {
        result = IoTHubClient_SetOption(iotHubClientHandle, OPTION_X509_CERT, deviceToUse->certificate);
        ASSERT_ARE_EQUAL_WITH_MSG(IOTHUB_CLIENT_RESULT, IOTHUB_CLIENT_OK, result, "Could not set the device x509 certificate");
        result = IoTHubClient_SetOption(iotHubClientHandle, OPTION_X509_PRIVATE_KEY, deviceToUse->primaryAuthentication);
        ASSERT_ARE_EQUAL_WITH_MSG(IOTHUB_CLIENT_RESULT, IOTHUB_CLIENT_OK, result, "Could not set the device x509 privateKey");
    }

    result = IoTHubClient_SetConnectionStatusCallback(iotHubClientHandle, connection_status_callback, &g_conn_info);
    ASSERT_ARE_EQUAL_WITH_MSG(IOTHUB_CLIENT_RESULT, IOTHUB_CLIENT_OK, result, "Could not set connection Status Callback");

    // Turn on Log 
    (void)IoTHubClient_SetOption(iotHubClientHandle, OPTION_LOG_TRACE, &trace);

    result = IoTHubClient_SetDeviceMethodCallback(iotHubClientHandle, DeviceMethodCallback, (void*)payload);
    ASSERT_ARE_EQUAL_WITH_MSG(IOTHUB_CLIENT_RESULT, IOTHUB_CLIENT_OK, result, "Could not set the device method callback");

    bool continue_running = true;
    beginOperation = time(NULL);
    do
    {
        if (Lock(g_conn_info.lock) != LOCK_OK)
        {
            ASSERT_FAIL("unable to lock");
        }
        else
        {
            if (g_conn_info.conn_status == IOTHUB_CLIENT_CONNECTION_AUTHENTICATED)
            {
                continue_running = false;
            }
            (void)Unlock(g_conn_info.lock);
        }

        ThreadAPI_Sleep(10);
    } while (
        (nowTime = time(NULL)),
        (difftime(nowTime, beginOperation) < IOTHUB_CONNECT_TIMEOUT_SEC) &&
        (continue_running)
        );
    ASSERT_ARE_EQUAL_WITH_MSG(IOTHUB_CLIENT_CONNECTION_STATUS, IOTHUB_CLIENT_CONNECTION_AUTHENTICATED, g_conn_info.conn_status, "Device Not connected");

    // Wait for the method to subscribe
    ThreadAPI_Sleep(1 * 1000);

    iotHubServiceClientHandle = IoTHubServiceClientAuth_CreateFromConnectionString(IoTHubAccount_GetIoTHubConnString(g_iothubAcctInfo));
    ASSERT_IS_NOT_NULL_WITH_MSG(iotHubServiceClientHandle, "Could not create service client handle");

    serviceClientDeviceMethodHandle = IoTHubDeviceMethod_Create(iotHubServiceClientHandle);
    ASSERT_IS_NOT_NULL_WITH_MSG(serviceClientDeviceMethodHandle, "Could not create device method handle");

    int responseStatus;
    unsigned char* responsePayload;
    size_t responsePayloadSize;
    IOTHUB_DEVICE_METHOD_RESULT invokeResult = IoTHubDeviceMethod_Invoke(serviceClientDeviceMethodHandle, deviceToUse->deviceId, METHOD_NAME, payload, TIMEOUT, &responseStatus, &responsePayload, &responsePayloadSize);

    ASSERT_ARE_EQUAL_WITH_MSG(IOTHUB_DEVICE_METHOD_RESULT, IOTHUB_DEVICE_METHOD_OK, invokeResult, "Service Client IoTHubDeviceMethod_Invoke failed");
    ASSERT_ARE_EQUAL_WITH_MSG(int, METHOD_RESPONSE_SUCCESS, responseStatus, "response status is incorrect");

    ASSERT_ARE_EQUAL_WITH_MSG(size_t, strlen(payload), responsePayloadSize, "response size is incorrect");
    if (memcmp(payload, responsePayload, responsePayloadSize))
    {
        ASSERT_FAIL("response string does not match");
    }

    // Send the Event from the client
    MAP_HANDLE propMap = Map_Create(NULL);

    if (Map_AddOrUpdate(propMap, "AzIoTHub_FaultOperationType", faultOperationType) != MAP_OK)
    {
        ASSERT_FAIL("Map_AddOrUpdate failed for AzIoTHub_FaultOperationType!");
    }

    if (Map_AddOrUpdate(propMap, "AzIoTHub_FaultOperationCloseReason", faultOperationCloseReason) != MAP_OK)
    {
        ASSERT_FAIL("Map_AddOrUpdate failed for AzIoTHub_FaultOperationCloseReason!");
    }

    if (Map_AddOrUpdate(propMap, "AzIoTHub_FaultOperationDelayInSecs", faultOperationDelayInSecs) != MAP_OK)
    {
        ASSERT_FAIL("Map_AddOrUpdate failed for AzIoTHub_FaultOperationDelayInSecs!");
    }

    (void)printf("Send fault control message...\r\n");
    client_create_with_properies_and_send_d2c(propMap);

    ThreadAPI_Sleep(3000);

    invokeResult = IoTHubDeviceMethod_Invoke(serviceClientDeviceMethodHandle, deviceToUse->deviceId, METHOD_NAME, payload, TIMEOUT, &responseStatus, &responsePayload, &responsePayloadSize);

    ASSERT_ARE_EQUAL_WITH_MSG(IOTHUB_DEVICE_METHOD_RESULT, IOTHUB_DEVICE_METHOD_OK, invokeResult, "Service Client IoTHubDeviceMethod_Invoke failed");
    ASSERT_ARE_EQUAL_WITH_MSG(int, METHOD_RESPONSE_SUCCESS, responseStatus, "response status is incorrect");

    ASSERT_ARE_EQUAL_WITH_MSG(size_t, strlen(payload), responsePayloadSize, "response size is incorrect");
    if (memcmp(payload, responsePayload, responsePayloadSize))
    {
        ASSERT_FAIL("response string does not match");
    }

    Map_Destroy(propMap);
    free(responsePayload);
}

void device_method_function_cleanup()
{
    if (serviceClientDeviceMethodHandle != NULL)
    {
        IoTHubDeviceMethod_Destroy(serviceClientDeviceMethodHandle);
        serviceClientDeviceMethodHandle = NULL;
    }

    if (iotHubServiceClientHandle != NULL)
    {
        IoTHubServiceClientAuth_Destroy(iotHubServiceClientHandle);
        iotHubServiceClientHandle = NULL;
    }

    if (iotHubClientHandle != NULL)
    {
        IoTHubClient_Destroy(iotHubClientHandle);
        iotHubClientHandle = NULL;
    }
}

void device_method_e2e_method_call_with_string_sas(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    test_device_method_with_string(IoTHubAccount_GetSASDevice(g_iothubAcctInfo), protocol, "\"I'm a happy little string\"");
}

void device_method_e2e_method_call_with_double_quoted_json_sas(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    test_device_method_with_string(IoTHubAccount_GetSASDevice(g_iothubAcctInfo), protocol, "{\"foo\":41,\"bar\":42,\"baz\":\"boo\"}");
}

void device_method_e2e_method_call_with_empty_json_object_sas(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    test_device_method_with_string(IoTHubAccount_GetSASDevice(g_iothubAcctInfo), protocol, "{}");
}

void device_method_e2e_method_call_with_null_sas(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    test_device_method_with_string(IoTHubAccount_GetSASDevice(g_iothubAcctInfo), protocol, "null");
}

extern void device_method_e2e_method_call_with_embedded_double_quote_sas(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    test_device_method_with_string(IoTHubAccount_GetSASDevice(g_iothubAcctInfo), protocol, "\"this string has a double quote \\\" in the middle\"");
}

extern void device_method_e2e_method_call_with_embedded_single_quote_sas(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    test_device_method_with_string(IoTHubAccount_GetSASDevice(g_iothubAcctInfo), protocol, "\"this string has a single quote ' in the middle\"");
}

void device_method_e2e_method_calls_upload_sas(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    test_device_method_calls_upload(IoTHubAccount_GetSASDevice(g_iothubAcctInfo), protocol, "\"Hello World.\"");
}

void device_method_e2e_method_call_with_string_x509(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    test_device_method_with_string(IoTHubAccount_GetX509Device(g_iothubAcctInfo), protocol, "\"I'm a happy little string\"");
}

void device_method_e2e_method_call_with_double_quoted_json_x509(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    test_device_method_with_string(IoTHubAccount_GetX509Device(g_iothubAcctInfo), protocol, "{\"foo\":41,\"bar\":42,\"baz\":\"boo\"}");
}

void device_method_e2e_method_call_with_empty_json_object_x509(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    test_device_method_with_string(IoTHubAccount_GetX509Device(g_iothubAcctInfo), protocol, "{}");
}

void device_method_e2e_method_call_with_null_x509(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    test_device_method_with_string(IoTHubAccount_GetX509Device(g_iothubAcctInfo), protocol, "null");
}

extern void device_method_e2e_method_call_with_embedded_double_quote_x509(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    test_device_method_with_string(IoTHubAccount_GetX509Device(g_iothubAcctInfo), protocol, "\"this string has a double quote \\\" in the middle\"");
}

extern void device_method_e2e_method_call_with_embedded_single_quote_x509(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    test_device_method_with_string(IoTHubAccount_GetX509Device(g_iothubAcctInfo), protocol, "\"this string has a single quote ' in the middle\"");
}

void device_method_e2e_method_calls_upload_x509(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    test_device_method_calls_upload(IoTHubAccount_GetX509Device(g_iothubAcctInfo), protocol, "\"Hello World.\"");
}

void device_method_e2e_method_call_svc_fault_ctrl_kill_Tcp(IOTHUB_CLIENT_TRANSPORT_PROVIDER protocol)
{
    test_device_method_with_string_svc_fault_ctrl(IoTHubAccount_GetSASDevice(g_iothubAcctInfo), protocol, "\"I'm a happy little string\"", "KillTcp", "boom", "1");
}
