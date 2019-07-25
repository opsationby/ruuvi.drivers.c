/**
 * Ruuvi BLE data advertising.
 *
 * License: BSD-3
 * Author: Otso Jousimaa <otso@ojousima.net>
 */

#include "ruuvi_driver_enabled_modules.h"
#if RUUVI_NRF5_SDK15_COMMUNICATION_BLE4_ADVERTISING_ENABLED

#include "ruuvi_driver_error.h"
#include "ruuvi_nrf5_sdk15_error.h"
#include "ruuvi_interface_communication.h"
#include "ruuvi_interface_communication_radio.h"
#include "ruuvi_interface_communication_ble4_advertising.h"
#include <stdint.h>

#include "nordic_common.h"
#include "nrf_nvic.h"
#include "nrf_soc.h"
#include "nrf_sdh.h"
#include "nrf_sdh_ble.h"
#include "ble_advdata.h"
#include "ble_nus.h"
#include "ble_types.h"
#include "sdk_errors.h"

typedef struct
{
  uint32_t advertisement_interval_ms;
  int8_t advertisement_power_dbm;
  uint16_t manufacturer_id;
  ruuvi_interface_communication_t* channel;
} ruuvi_platform_ble4_advertisement_state_t;

/** Buffers for scan data. Data has to be double-buffered for live switching of data */
static uint8_t  m_advertisement0[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
static uint16_t m_adv0_len;
static uint8_t  m_advertisement1[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
static uint16_t m_adv1_len;
static uint8_t  m_scan0[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
static uint16_t m_scan0_len;
static uint8_t  m_scan1[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
static uint16_t m_scan1_len;

static bool advertisement_odd = false;

static ble_gap_adv_data_t m_adv_data;

static ble_gap_conn_sec_mode_t m_security;

// TODO: Define somewhere else. SDK_APPLICATION_CONFIG?
#define DEFAULT_ADV_INTERVAL_MS 1010
#define MIN_ADV_INTERVAL_MS     100
#define MAX_ADV_INTERVAL_MS     10000
 /** @brief Parameters to be passed to the stack when starting advertising. */
static ble_gap_adv_params_t m_adv_params;
/** @brief Advertising handle used to identify an advertising set. */
static uint8_t              m_adv_handle = BLE_GAP_ADV_SET_HANDLE_NOT_SET;
/** @brief Flag for initialization **/
static bool                 m_advertisement_is_init = false;
/** @brief Flag for advertising in process **/
static bool                 m_advertising = false;
ruuvi_platform_ble4_advertisement_state_t m_adv_state;

 /**< Universally unique service identifier of Nordic UART Service */
static ble_uuid_t m_adv_uuids[] =                       
{
  {BLE_UUID_NUS_SERVICE, BLE_UUID_TYPE_VENDOR_BEGIN}
};

// Update BLE settings, takes effect immidiately
static ruuvi_driver_status_t update_settings(void)
{
  if(!m_advertisement_is_init) { return RUUVI_DRIVER_ERROR_INVALID_STATE; }

  ret_code_t err_code = NRF_SUCCESS;

  // Stop advertising for setting update
  if(m_advertising)
  {
    err_code |= sd_ble_gap_adv_stop(m_adv_handle);
  }

  err_code |= sd_ble_gap_adv_set_configure(&m_adv_handle, &m_adv_data, &m_adv_params);

  if(m_advertising)
  {
    err_code = sd_ble_gap_adv_start(m_adv_handle, RUUVI_NRF5_SDK15_BLE4_STACK_CONN_TAG);
  }

  return ruuvi_nrf5_sdk15_to_ruuvi_error(err_code);
}

/*
 * Assume that radio activity was caused by this module and call event handler with sent-event
 */
void ruuvi_interface_communication_ble4_advertising_activity_handler(
  const ruuvi_interface_communication_radio_activity_evt_t evt)
{
  // Before activity - no action
  if(RUUVI_INTERFACE_COMMUNICATION_RADIO_BEFORE == evt) { return; }

  // After activity - assume that all activity is related to advertisement tx
  if(RUUVI_INTERFACE_COMMUNICATION_RADIO_AFTER == evt)
  {
    if(NULL != m_adv_state.channel->on_evt)
    {
      // TODO: Add information about sent advertisement
      m_adv_state.channel->on_evt(RUUVI_INTERFACE_COMMUNICATION_SENT, NULL, 0);
    }
  }
}

ruuvi_driver_status_t ruuvi_interface_communication_ble4_advertising_tx_interval_set(
  const uint32_t ms)
{
  if(MIN_ADV_INTERVAL_MS > ms || MAX_ADV_INTERVAL_MS < ms) { return RUUVI_DRIVER_ERROR_INVALID_PARAM; }

  m_adv_state.advertisement_interval_ms = ms;
  m_adv_params.interval = MSEC_TO_UNITS(m_adv_state.advertisement_interval_ms,
                                        UNIT_0_625_MS);
  return update_settings();
}

ruuvi_driver_status_t ruuvi_interface_communication_ble4_advertising_tx_interval_get(
  uint32_t* ms)
{
  *ms = m_adv_state.advertisement_interval_ms;
  return RUUVI_DRIVER_SUCCESS;
}

ruuvi_driver_status_t ruuvi_interface_communication_ble4_advertising_manufacturer_id_set(
  const uint16_t id)
{
  m_adv_state.manufacturer_id = id;
  return RUUVI_DRIVER_SUCCESS;
}

/*
 * Initializes radio hardware, advertising module and scanning module
 *
 * Returns RUUVI_DIRVER_SUCCESS on success, RUUVI_DIRVER_ERROR_INVALID_STATE if radio is already initialized
 */
ruuvi_driver_status_t ruuvi_interface_communication_ble4_advertising_init(
  ruuvi_interface_communication_t* const channel)
{
  ruuvi_driver_status_t err_code = RUUVI_DRIVER_SUCCESS;

  if(!m_advertisement_is_init)
  {
    err_code |= ruuvi_interface_communication_radio_init(
                  RUUVI_INTERFACE_COMMUNICATION_RADIO_ADVERTISEMENT);

    if(RUUVI_DRIVER_SUCCESS != err_code) { return err_code; }
  }

  // Initialize advertising parameters (used when starting advertising).
  memset(&m_adv_params, 0, sizeof(m_adv_params));
  m_adv_params.filter_policy   = BLE_GAP_ADV_FP_ANY;
  m_adv_params.duration        = 0;       // Never time out.
  m_adv_params.properties.type = BLE_GAP_ADV_TYPE_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED;
  m_adv_params.p_peer_addr     = NULL;    // Undirected advertisement.
  m_adv_params.interval        = MSEC_TO_UNITS(DEFAULT_ADV_INTERVAL_MS, UNIT_0_625_MS);
  m_advertisement_is_init = true;
  m_adv_state.advertisement_interval_ms = DEFAULT_ADV_INTERVAL_MS;
  m_adv_state.channel = channel;
  channel->init    = ruuvi_interface_communication_ble4_advertising_init;
  channel->uninit  = ruuvi_interface_communication_ble4_advertising_uninit;
  channel->send    = ruuvi_interface_communication_ble4_advertising_send;
  channel->read    = ruuvi_interface_communication_ble4_advertising_receive;
  channel->on_evt  = NULL;
  memset(&m_adv_data, 0, sizeof(m_adv_data));
  memset(&m_advertisement0, 0, sizeof(m_advertisement0));
  memset(&m_advertisement1, 0, sizeof(m_advertisement1));
  m_adv0_len = 0;
  m_adv1_len = 0;
  return ruuvi_nrf5_sdk15_to_ruuvi_error(err_code);
}

/*
 * Uninitializes radio hardware, advertising module and scanning module
 *
 * Returns RUUVI_DIRVER_SUCCESS on success or if radio was not initialized.
 * Returns RUUVI_DRIVER_ERROR_INVALID_STATE if radio hardware was initialized by another radio module.
 */
ruuvi_driver_status_t ruuvi_interface_communication_ble4_advertising_uninit(
  ruuvi_interface_communication_t* const channel)
{
  ruuvi_driver_status_t err_code = RUUVI_DRIVER_SUCCESS;

  // Stop advertising
  if(true == m_advertising)
  {
    sd_ble_gap_adv_stop(m_adv_handle);
    m_advertising = false;
  }

  // Clear advertisement parameters
  memset(&m_adv_params, 0, sizeof(m_adv_params));
  // Release radio
  err_code |= ruuvi_interface_communication_radio_uninit(
                RUUVI_INTERFACE_COMMUNICATION_RADIO_ADVERTISEMENT);
  m_advertisement_is_init = false;
  // Clear function pointers
  memset(channel, 0, sizeof(ruuvi_interface_communication_t));
  memset(&m_adv_state, 0, sizeof(m_adv_state));
  return err_code;
}


// Not implemented
//ruuvi_driver_status_t ruuvi_interface_communication_ble4_advertising_rx_interval_set(uint32_t* window_interval_ms, uint32_t* window_size_ms);
//ruuvi_driver_status_t ruuvi_interface_communication_ble4_advertising_rx_interval_get(uint32_t* window_interval_ms, uint32_t* window_size_ms);

// Set manufacturer specific data to advertise. Clears previous data.
ruuvi_driver_status_t ruuvi_interface_communication_ble4_advertising_data_set(
  const uint8_t* data, const uint8_t data_length)
{
  if(NULL == data)     { return RUUVI_DRIVER_ERROR_NULL; }

  if(24 < data_length) { return RUUVI_DRIVER_ERROR_INVALID_LENGTH; }

  // Build specification for data into ble_advdata_t advdata
  ble_advdata_t advdata = {0};
  // Only valid flag
  uint8_t       flags = BLE_GAP_ADV_FLAG_BR_EDR_NOT_SUPPORTED;
  // Build manufacturer specific data
  ble_advdata_manuf_data_t manuf_specific_data;
  ret_code_t err_code = NRF_SUCCESS;
  // Preserve const of data passed to us.
  uint8_t manufacturer_data[24];
  memcpy(manufacturer_data, data, data_length);
  manuf_specific_data.data.p_data = manufacturer_data;
  manuf_specific_data.data.size   = data_length;
  manuf_specific_data.company_identifier = m_adv_state.manufacturer_id;
  // Point to manufacturer data and flags set earlier
  advdata.flags                 = flags;
  advdata.p_manuf_specific_data = &manuf_specific_data;

  // If manufacturer data is not set, assign "UNKNOWN"
  if(0 == m_adv_state.manufacturer_id) { manuf_specific_data.company_identifier = 0xFFFF; }

  // Same buffer must not be passed to the SD on data update.
  ble_gap_adv_data_t* p_adv_data = &m_adv_data;
  uint8_t* p_advertisement       = (advertisement_odd) ? m_advertisement0 :
                                   m_advertisement1;
  uint16_t* p_adv_len            = (advertisement_odd) ? &m_adv0_len      : &m_adv1_len;
  uint8_t* p_scan                = (advertisement_odd) ? m_scan0 :
                                   m_scan1;
  uint16_t* p_scan_len           = (advertisement_odd) ? &m_scan0_len     : &m_scan1_len;
  m_adv0_len = sizeof(m_advertisement0);
  m_adv1_len = sizeof(m_advertisement1);
  m_scan0_len = sizeof(m_scan0);
  m_scan1_len = sizeof(m_scan1);
  advertisement_odd = !advertisement_odd;
  // Encode data
  err_code |= ble_advdata_encode(&advdata, p_advertisement, p_adv_len);
  p_adv_data->adv_data.p_data     = p_advertisement;
  p_adv_data->adv_data.len        = *p_adv_len;
  p_adv_data->scan_rsp_data.p_data = p_scan;
  p_adv_data->scan_rsp_data.len    = *p_scan_len;

  err_code |= sd_ble_gap_adv_set_configure(&m_adv_handle, p_adv_data, NULL);
  return ruuvi_nrf5_sdk15_to_ruuvi_error(err_code);
}

/**
 * Send data as manufacturer specific data payload.
 * If no new data is placed to the buffer, last message sent will be repeated.
 *
 * Returns RUUVI_DRIVER_SUCCESS if the data was queued to Softdevice
 * Returns RUUVI_DRIVER_ERROR_NULL if the data was null.
 * Returns RUUVI_DRIVER_ERROR_INVALID_LENGTH if data length is over 24 bytes
 */
ruuvi_driver_status_t ruuvi_interface_communication_ble4_advertising_send(
  ruuvi_interface_communication_message_t* message)
{

  if(NULL == message) { return RUUVI_DRIVER_ERROR_NULL; }
  
  // Advertising might be stopped by external event, such as GATT connection.
  if(false == m_advertising)
  {
    return RUUVI_DRIVER_ERROR_INVALID_STATE;
  }

  return ruuvi_interface_communication_ble4_advertising_data_set(message->data,
                                                                 message->data_length);
}

// Not implemented
ruuvi_driver_status_t ruuvi_interface_communication_ble4_advertising_receive(
  ruuvi_interface_communication_message_t* message)
{
  return RUUVI_DRIVER_ERROR_NOT_IMPLEMENTED;
}

// TODO: Device-specific TX powers
ruuvi_driver_status_t ruuvi_interface_communication_ble4_advertising_tx_power_set(
  int8_t* dbm)
{
  int8_t  tx_power = 0;
  ret_code_t err_code = NRF_SUCCESS;

  if(*dbm <= -40) { tx_power = -40; }
  else if(*dbm <= -20) { tx_power = -20; }
  else if(*dbm <= -16) { tx_power = -16; }
  else if(*dbm <= -12) { tx_power = -12; }
  else if(*dbm <= -8) { tx_power = -8; }
  else if(*dbm <= -4) { tx_power = -4; }
  else if(*dbm <= 0) { tx_power = 0; }
  else if(*dbm <= 4) { tx_power = 4; }
  else { return RUUVI_DRIVER_ERROR_INVALID_PARAM; }

  err_code = sd_ble_gap_tx_power_set(BLE_GAP_TX_POWER_ROLE_ADV,
                                     m_adv_handle,
                                     tx_power
                                    );
  return ruuvi_nrf5_sdk15_to_ruuvi_error(err_code);
}


ruuvi_driver_status_t ruuvi_interface_communication_ble4_advertising_tx_power_get(
  int8_t* dbm)
{
  return RUUVI_DRIVER_ERROR_NOT_IMPLEMENTED;
}

ruuvi_driver_status_t ruuvi_interface_communication_ble4_advertising_scan_response_setup
  (const char* const name,
  const bool advertise_nus)
{
  ruuvi_driver_status_t err_code = RUUVI_DRIVER_SUCCESS;
  

  memset(&m_scan0, 0, sizeof(m_scan0));
  memset(&m_scan1, 0, sizeof(m_scan1));
  ble_advdata_t scanrsp = {0};

  if(NULL != name)
  {
    // Name will be read from the GAP data
    scanrsp.name_type = BLE_ADVDATA_FULL_NAME;
    uint8_t len = strlen(name);
    err_code |= sd_ble_gap_device_name_set(&m_security, (uint8_t*)name, len);
  }

  // Add scan response
  if(advertise_nus)
  {
    scanrsp.uuids_complete.uuid_cnt = 1;
    scanrsp.uuids_complete.p_uuids = &(m_adv_uuids[0]);
  }

  // Encode data
  m_adv_data.scan_rsp_data.len = sizeof(m_scan0);
  err_code |= ble_advdata_encode(&scanrsp, m_scan0, &m_adv_data.scan_rsp_data.len);
  err_code |= ble_advdata_encode(&scanrsp, m_scan1, &m_adv_data.scan_rsp_data.len);

  return ruuvi_nrf5_sdk15_to_ruuvi_error(err_code);
}



ruuvi_driver_status_t ruuvi_interface_communication_ble4_advertising_type_set(RUUVI_INTERFACE_COMMUNICATION_BLE4_ADVERTISING_TYPE type)
{
  ruuvi_driver_status_t err_code = RUUVI_DRIVER_SUCCESS;
  switch(type)
  {
    case NONCONNECTABLE_NONSCANNABLE:
      m_adv_params.properties.type = BLE_GAP_ADV_TYPE_NONCONNECTABLE_NONSCANNABLE_UNDIRECTED;
      break;

    case NONCONNECTABLE_SCANNABLE:
      m_adv_params.properties.type = BLE_GAP_ADV_TYPE_NONCONNECTABLE_SCANNABLE_UNDIRECTED;
      break;

    case CONNECTABLE_NONSCANNABLE:
      m_adv_params.properties.type = BLE_GAP_ADV_TYPE_EXTENDED_CONNECTABLE_NONSCANNABLE_UNDIRECTED;
      break;

    case CONNECTABLE_SCANNABLE:
      m_adv_params.properties.type = BLE_GAP_ADV_TYPE_CONNECTABLE_SCANNABLE_UNDIRECTED;
      break;

    default:
      err_code = RUUVI_DRIVER_ERROR_INVALID_PARAM;
  }
  return err_code;
}

void ruuvi_interface_communication_ble4_advertising_notify_stop(void)
{
  m_advertising = false;
}

ruuvi_driver_status_t ruuvi_interface_communication_ble4_advertising_start()
{
  ruuvi_driver_status_t err_code = RUUVI_DRIVER_SUCCESS;
  err_code |= sd_ble_gap_adv_set_configure(&m_adv_handle, NULL, &m_adv_params);
  err_code |= ruuvi_nrf5_sdk15_to_ruuvi_error(sd_ble_gap_adv_start(m_adv_handle, RUUVI_NRF5_SDK15_BLE4_STACK_CONN_TAG));
  if(RUUVI_DRIVER_SUCCESS == err_code) { m_advertising = true; }
  return err_code;
}
ruuvi_driver_status_t ruuvi_interface_communication_ble4_advertising_stop()
{
  ruuvi_driver_status_t err_code = RUUVI_DRIVER_SUCCESS;
  err_code |= ruuvi_nrf5_sdk15_to_ruuvi_error(sd_ble_gap_adv_stop(RUUVI_NRF5_SDK15_BLE4_STACK_CONN_TAG));
  if(RUUVI_DRIVER_SUCCESS == err_code) { m_advertising = false; }
  return err_code;
}

#endif