/*
 * Copyright (c) 2012 Nordic Semiconductor. All Rights Reserved.
 *
 * The information contained herein is confidential property of Nordic Semiconductor. The use,
 * copying, transfer or disclosure of such information is prohibited except by express written
 * agreement with Nordic Semiconductor.
 *
 */

#include "ble_bas_c.h"
#include "ble_db_discovery.h"
#include "ble_types.h"
#include "ble_srv_common.h"
#include "ble_gattc.h"
#include "app_trace.h"
#include "sdk_common.h"

#define THROUGHPUT_TEST_BAS 1

#define LOG                  app_trace_log         /**< Debug logger macro that will be used in this file to do logging of important information over UART. */
#define TX_BUFFER_MASK       0x07                  /**< TX Buffer mask, must be a mask of contiguous zeroes, followed by contiguous sequence of ones: 000...111. */
#define TX_BUFFER_SIZE       (TX_BUFFER_MASK + 1)  /**< Size of the send buffer, which is 1 higher than the mask. */
#define WRITE_MESSAGE_LENGTH BLE_CCCD_VALUE_LEN    /**< Length of the write message for CCCD. */

typedef enum
{
    READ_REQ,      /**< Type identifying that this tx_message is a read request. */
    WRITE_REQ      /**< Type identifying that this tx_message is a write request. */
} tx_request_t;

/**@brief Structure for writing a message to the peer, i.e. CCCD.
 */
typedef struct
{
    uint8_t                  gattc_value[WRITE_MESSAGE_LENGTH];  /**< The message to write. */
    ble_gattc_write_params_t gattc_params;                       /**< The GATTC parameters for this message. */
} write_params_t;

/**@brief Structure for holding the data that will be transmitted to the connected central.
 */
typedef struct
{
    uint16_t     conn_handle;  /**< Connection handle to be used when transmitting this message. */
    tx_request_t type;         /**< Type of message. (read or write). */
    union
    {
        uint16_t       read_handle;  /**< Read request handle. */
        write_params_t write_req;    /**< Write request message. */
    } req;
} tx_message_t;


static ble_bas_c_t * mp_ble_bas_c;                 /**< Pointer to the current instance of the BAS Client module. The memory for this is provided by the application.*/
static tx_message_t  m_tx_buffer[TX_BUFFER_SIZE];  /**< Transmit buffer for the messages that will be transmitted to the central. */
static uint32_t      m_tx_insert_index = 0;        /**< Current index in the transmit buffer where the next message should be inserted. */
static uint32_t      m_tx_index        = 0;        /**< Current index in the transmit buffer containing the next message to be transmitted. */

#if THROUGHPUT_TEST_BAS
/* Marcus add, use to reveice test data */
extern uint8_t testData[TEST_DATA_SIZE];
#endif

/**@brief Function for passing any pending request from the buffer to the stack.
 */
static void tx_buffer_process(void)
{
    if (m_tx_index != m_tx_insert_index)
    {
        uint32_t err_code;

        if (m_tx_buffer[m_tx_index].type == READ_REQ)
        {
            err_code = sd_ble_gattc_read(m_tx_buffer[m_tx_index].conn_handle,
                                         m_tx_buffer[m_tx_index].req.read_handle,
                                         0);
        }
        else
        {
            err_code = sd_ble_gattc_write(m_tx_buffer[m_tx_index].conn_handle,
                                          &m_tx_buffer[m_tx_index].req.write_req.gattc_params);
        }
        if (err_code == NRF_SUCCESS)
        {
            LOG("[BAS_C]: SD Read/Write API returns Success..\r\n");
            m_tx_index++;
            m_tx_index &= TX_BUFFER_MASK;
        }
        else
        {
            LOG("[BAS_C]: SD Read/Write API returns error. This message sending will be "
                "attempted again..\r\n");
        }
    }
}


/**@brief Function for handling write response events.
 *
 * @param[in] p_bas_c   Pointer to the Battery Service Client Structure.
 * @param[in] p_ble_evt Pointer to the SoftDevice event.
 */
static void on_write_rsp(ble_bas_c_t * p_bas_c, const ble_evt_t * p_ble_evt)
{
    // Check if there is any message to be sent across to the peer and send it.
    tx_buffer_process();
}


/**@brief     Function for handling read response events.
 *
 * @details   This function will validate the read response and raise the appropriate
 *            event to the application.
 *
 * @param[in] p_bas_c   Pointer to the Battery Service Client Structure.
 * @param[in] p_ble_evt Pointer to the SoftDevice event.
 */
static void on_read_rsp(ble_bas_c_t * p_bas_c, const ble_evt_t * p_ble_evt)
{
    const ble_gattc_evt_read_rsp_t * p_response;

    p_response = &p_ble_evt->evt.gattc_evt.params.read_rsp;

    if (p_response->handle == p_bas_c->bl_handle)
    {
        ble_bas_c_evt_t evt;

        evt.evt_type = BLE_BAS_C_EVT_BATT_READ_RESP;

        evt.params.battery_level = p_response->data[0];

        p_bas_c->evt_handler(p_bas_c, &evt);
    }
    // Check if there is any buffered transmissions and send them.
    tx_buffer_process();
}


/**@brief     Function for handling Handle Value Notification received from the SoftDevice.
 *
 * @details   This function will handle the Handle Value Notification received from the SoftDevice
 *            and checks if it is a notification of the Battery Level measurement from the peer. If
 *            so, this function will decode the battery level measurement and send it to the
 *            application.
 *
 * @param[in] p_ble_bas_c Pointer to the Battery Service Client structure.
 * @param[in] p_ble_evt   Pointer to the BLE event received.
 */
static void on_hvx(ble_bas_c_t * p_ble_bas_c, const ble_evt_t * p_ble_evt)
{
    // Check if this notification is a battery level notification.
    if (p_ble_evt->evt.gattc_evt.params.hvx.handle == p_ble_bas_c->bl_handle)
    {
        ble_bas_c_evt_t ble_bas_c_evt;

        ble_bas_c_evt.evt_type = BLE_BAS_C_EVT_BATT_NOTIFICATION;

        ble_bas_c_evt.params.battery_level = p_ble_evt->evt.gattc_evt.params.hvx.data[0];
#if THROUGHPUT_TEST_BAS
/* Record the received notify data */
				for (uint8_t i = 0; i < TEST_DATA_SIZE; i++)
					testData[i] = p_ble_evt->evt.gattc_evt.params.hvx.data[i];
#endif
        p_ble_bas_c->evt_handler(p_ble_bas_c, &ble_bas_c_evt);
    }
}


/**@brief     Function for handling events from the database discovery module.
 *
 * @details   This function will handle an event from the database discovery module, and determine
 *            if it relates to the discovery of Battery service at the peer. If so, it will
 *            call the application's event handler indicating that the Battery service has been
 *            discovered at the peer. It also populate the event with the service related
 *            information before providing it to the application.
 *
 * @param[in] p_evt Pointer to the event received from the database discovery module.
 *
 */
static void db_discovery_evt_handler(ble_db_discovery_evt_t * p_evt)
{
    // Check if the Battery Service was discovered.
    if (p_evt->evt_type == BLE_DB_DISCOVERY_COMPLETE
        &&
        p_evt->params.discovered_db.srv_uuid.uuid == BLE_UUID_BATTERY_SERVICE
        &&
        p_evt->params.discovered_db.srv_uuid.type == BLE_UUID_TYPE_BLE)
    {
        mp_ble_bas_c->conn_handle = p_evt->conn_handle;

        // Find the CCCD Handle of the Battery Level characteristic.
        uint8_t i;

        for (i = 0; i < p_evt->params.discovered_db.char_count; i++)
        {
            if (p_evt->params.discovered_db.charateristics[i].characteristic.uuid.uuid ==
                BLE_UUID_BATTERY_LEVEL_CHAR)
            {
                // Found Battery Level characteristic. Store CCCD handle and break.
                mp_ble_bas_c->bl_cccd_handle =
                    p_evt->params.discovered_db.charateristics[i].cccd_handle;
                mp_ble_bas_c->bl_handle =
                    p_evt->params.discovered_db.charateristics[i].characteristic.handle_value;
                break;
            }
        }

        LOG("[BAS_C]: Battery Service discovered at peer.\r\n");

        ble_bas_c_evt_t evt;

        evt.evt_type = BLE_BAS_C_EVT_DISCOVERY_COMPLETE;

        mp_ble_bas_c->evt_handler(mp_ble_bas_c, &evt);
    }
    else
    {
        LOG("[BAS_C]: Battery Service discovery failure at peer. \r\n");
    }
}


/**@brief Function for creating a message for writing to the CCCD.
 */
static uint32_t cccd_configure(uint16_t conn_handle, uint16_t handle_cccd, bool notification_enable)
{
    LOG("[BAS_C]: Configuring CCCD. CCCD Handle = %d, Connection Handle = %d\r\n",
                                                            handle_cccd,conn_handle);

    tx_message_t * p_msg;
    uint16_t       cccd_val = notification_enable ? BLE_GATT_HVX_NOTIFICATION : 0;

    p_msg              = &m_tx_buffer[m_tx_insert_index++];
    m_tx_insert_index &= TX_BUFFER_MASK;

    p_msg->req.write_req.gattc_params.handle   = handle_cccd;
    p_msg->req.write_req.gattc_params.len      = WRITE_MESSAGE_LENGTH;
    p_msg->req.write_req.gattc_params.p_value  = p_msg->req.write_req.gattc_value;
    p_msg->req.write_req.gattc_params.offset   = 0;
    p_msg->req.write_req.gattc_params.write_op = BLE_GATT_OP_WRITE_REQ;
    p_msg->req.write_req.gattc_value[0]        = LSB_16(cccd_val);
    p_msg->req.write_req.gattc_value[1]        = MSB_16(cccd_val);
    p_msg->conn_handle                         = conn_handle;
    p_msg->type                                = WRITE_REQ;

    tx_buffer_process();
    return NRF_SUCCESS;
}


uint32_t ble_bas_c_init(ble_bas_c_t * p_ble_bas_c, ble_bas_c_init_t * p_ble_bas_c_init)
{
    VERIFY_PARAM_NOT_NULL(p_ble_bas_c);
    VERIFY_PARAM_NOT_NULL(p_ble_bas_c_init);

    ble_uuid_t bas_uuid;

    bas_uuid.type                = BLE_UUID_TYPE_BLE;
    bas_uuid.uuid                = BLE_UUID_BATTERY_SERVICE;

    mp_ble_bas_c                 = p_ble_bas_c;

    mp_ble_bas_c->conn_handle    = BLE_CONN_HANDLE_INVALID;
    mp_ble_bas_c->bl_cccd_handle = BLE_GATT_HANDLE_INVALID;
    mp_ble_bas_c->bl_handle      = BLE_GATT_HANDLE_INVALID;
    mp_ble_bas_c->evt_handler    = p_ble_bas_c_init->evt_handler;

    return ble_db_discovery_evt_register(&bas_uuid,
                                         db_discovery_evt_handler);
}


void ble_bas_c_on_ble_evt(ble_bas_c_t * p_ble_bas_c, const ble_evt_t * p_ble_evt)
{
    if ((p_ble_bas_c == NULL) || (p_ble_evt == NULL))
    {
        return;
    }

    switch (p_ble_evt->header.evt_id)
    {
        case BLE_GAP_EVT_CONNECTED:
            p_ble_bas_c->conn_handle = p_ble_evt->evt.gap_evt.conn_handle;
            break;

        case BLE_GATTC_EVT_HVX:
            on_hvx(p_ble_bas_c, p_ble_evt);
            break;

        case BLE_GATTC_EVT_WRITE_RSP:
            on_write_rsp(p_ble_bas_c, p_ble_evt);
            break;

        case BLE_GATTC_EVT_READ_RSP:
            on_read_rsp(p_ble_bas_c, p_ble_evt);
            break;

        default:
            break;
    }
}


uint32_t ble_bas_c_bl_notif_enable(ble_bas_c_t * p_ble_bas_c)
{
    VERIFY_PARAM_NOT_NULL(p_ble_bas_c);

    return cccd_configure(p_ble_bas_c->conn_handle, p_ble_bas_c->bl_cccd_handle, true);
}


uint32_t ble_bas_c_bl_read(ble_bas_c_t * p_ble_bas_c)
{
    tx_message_t * msg;

    msg                  = &m_tx_buffer[m_tx_insert_index++];
    m_tx_insert_index   &= TX_BUFFER_MASK;

    msg->req.read_handle = p_ble_bas_c->bl_handle;
    msg->conn_handle     = p_ble_bas_c->conn_handle;
    msg->type            = READ_REQ;

    tx_buffer_process();
    return NRF_SUCCESS;
}
