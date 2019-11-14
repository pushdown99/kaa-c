#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <kaa.h>
#include <platform/kaa_client.h>
#include <kaa_error.h>
#include <kaa_configuration_manager.h>
#include <kaa_logging.h>
#include <gen/kaa_logging_gen.h>
#include <platform/kaa_client.h>
#include <utilities/kaa_log.h>

#include <platform-impl/common/ext_log_upload_strategies.h>

////////////////////////////////////////////////////////////////////////////////////////////////////
#include "MQTTAsync.h"
#include "MQTTClientPersistence.h"

#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>

#include <sys/time.h>
#include <unistd.h>

volatile int finished = 0;
int subscribed = 0;
int disconnected = 0;

char *topic     = "kaa/12/1/11/0";
char *clientid  = "paho-c-sub";
int qos = 0;
// hyhwang
int keepalive = 10; //10;
int MQTTVersion = MQTTVERSION_DEFAULT;

pthread_t t_mqtt;

int32_t temperature = 0;

int messageArrived(void *context, char *topicName, int topicLen, MQTTAsync_message *message)
{
    printf("%d %s\t", message->payloadlen, topicName);
    printf("%.*s\n", message->payloadlen, (char*)message->payload);
    char buf[BUFSIZ] = "";
    sprintf(buf, "%.*s\n", message->payloadlen, (char*)message->payload);
    temperature = atoi(buf);
    fflush(stdout);


    MQTTAsync_freeMessage(&message);
    MQTTAsync_free(topicName);
    return 1;
}


void onDisconnect(void* context, MQTTAsync_successData* response)
{
    disconnected = 1;
}

void onSubscribe(void* context, MQTTAsync_successData* response)
{
    subscribed = 1;
}

void onSubscribeFailure(void* context, MQTTAsync_failureData* response)
{
    fprintf(stderr, "Subscribe failed, rc %s\n", MQTTAsync_strerror(response->code));
    finished = 1;
}


void onConnectFailure(void* context, MQTTAsync_failureData* response)
{
    fprintf(stderr, "Connect failed, rc %s\n", response ? MQTTAsync_strerror(response->code) : "none");
    finished = 1;
}

void onConnect(void* context, MQTTAsync_successData* response)
{
    MQTTAsync client = (MQTTAsync)context;
    MQTTAsync_responseOptions ropts = MQTTAsync_responseOptions_initializer;
    int rc;

printf("%s \n",__FUNCTION__);
    printf("Subscribing to topic %s with client %s at QoS %d\n", topic, clientid, qos);

    ropts.onSuccess = onSubscribe;
    ropts.onFailure = onSubscribeFailure;
    ropts.context = client;
    if ((rc = MQTTAsync_subscribe(client, topic, qos, &ropts)) != MQTTASYNC_SUCCESS)
    {
        fprintf(stderr, "Failed to start subscribe, return code %s\n", MQTTAsync_strerror(rc));
        finished = 1;
    }
}

MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer;


static void*
mqtt_task(void* args)
{
    MQTTAsync client;
    MQTTAsync_disconnectOptions disc_opts = MQTTAsync_disconnectOptions_initializer;
    MQTTAsync_createOptions create_opts = MQTTAsync_createOptions_initializer;
    int rc = 0;

    rc = MQTTAsync_createWithOptions(&client, "222.112.107.46:1883", clientid, MQTTCLIENT_PERSISTENCE_NONE,
            NULL, &create_opts);
    if (rc != MQTTASYNC_SUCCESS)
    {
        fprintf(stderr, "Failed to create client, return code: %s\n", MQTTAsync_strerror(rc));
        exit(EXIT_FAILURE);
    }

    rc = MQTTAsync_setCallbacks(client, client, NULL, messageArrived, NULL);
    if (rc != MQTTASYNC_SUCCESS)
    {
        fprintf(stderr, "Failed to set callbacks, return code: %s\n", MQTTAsync_strerror(rc));
        exit(EXIT_FAILURE);
    }

    conn_opts.onSuccess = onConnect;
    conn_opts.onFailure = onConnectFailure;
    conn_opts.cleansession = 1;
    conn_opts.keepAliveInterval = keepalive;
    conn_opts.MQTTVersion = MQTTVersion;
    conn_opts.context = client;
    conn_opts.automaticReconnect = 1;

    if ((rc = MQTTAsync_connect(client, &conn_opts)) != MQTTASYNC_SUCCESS)
    {
        fprintf(stderr, "Failed to start connect, return code %s\n", MQTTAsync_strerror(rc));
        exit(EXIT_FAILURE);
    }
    while (!subscribed) usleep(100 * 1000);

    if (finished)
        goto exit;

    while (!finished) usleep(100 * 1000);

    disc_opts.onSuccess = onDisconnect;
    if ((rc = MQTTAsync_disconnect(client, &disc_opts)) != MQTTASYNC_SUCCESS)
    {
        fprintf(stderr, "Failed to start disconnect, return code: %s\n", MQTTAsync_strerror(rc));
        exit(EXIT_FAILURE);
    }
    while (!disconnected) usleep(100 * 1000);

exit:
    MQTTAsync_destroy(&client);
    exit(EXIT_SUCCESS);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
static int32_t sample_period;
static time_t  last_sample_time;
extern kaa_error_t ext_unlimited_log_storage_create(void **log_storage_context_p, kaa_logger_t *logger);
/* Periodically called by Kaa SDK. */
static void example_callback(void *context)
{
    time_t current_time = time(NULL);
    /* Respect sample period */
    if (difftime(current_time, last_sample_time) >= sample_period) {
        last_sample_time = current_time;
        kaa_user_log_record_t *log_record = kaa_logging_data_collection_create();
        log_record->temperature = temperature;
        kaa_logging_add_record(kaa_client_get_context(context)->log_collector, log_record, NULL);
    }
}
/* Receives new configuration data. */
static kaa_error_t on_configuration_updated(void *context, const kaa_root_configuration_t *conf)
{
    (void) context;
    printf("Received configuration data. New sample period: %i seconds\n", conf->sample_period);
    sample_period = conf->sample_period;
    return KAA_ERR_NONE;
}

int main(void)
{
    pthread_create(&t_mqtt, NULL, mqtt_task, NULL);

    /* Init random generator used to generate temperature */
    srand(time(NULL));
    /* Prepare Kaa client. */
    kaa_client_t *kaa_client = NULL;
    kaa_error_t error = kaa_client_create(&kaa_client, NULL);
    if (error) {
        return EXIT_FAILURE;
    }
    /* Configure notification manager. */
    kaa_configuration_root_receiver_t receiver = {
        .context = NULL,
        .on_configuration_updated = on_configuration_updated
    };
    error = kaa_configuration_manager_set_root_receiver(
        kaa_client_get_context(kaa_client)->configuration_manager,
        &receiver);
    if (error) {
        return EXIT_FAILURE;
    }
    /* Obtain default configuration shipped within SDK. */
    const kaa_root_configuration_t *dflt = kaa_configuration_manager_get_configuration(
        kaa_client_get_context(kaa_client)->configuration_manager);
    printf("Default sample period: %i seconds\n", dflt->sample_period);
    sample_period = dflt->sample_period;
    
    /* Configure data collection. */
    void *log_storage_context         = NULL;
    void *log_upload_strategy_context = NULL;
    /* The internal memory log storage distributed with Kaa SDK. */
    error = ext_unlimited_log_storage_create(&log_storage_context,
        kaa_client_get_context(kaa_client)->logger);
    if (error) {
        return EXIT_FAILURE;
    }
    /* Create a strategy based on timeout. */
    error = ext_log_upload_strategy_create(
        kaa_client_get_context(kaa_client), &log_upload_strategy_context,
        KAA_LOG_UPLOAD_BY_TIMEOUT_STRATEGY);
    if (error) {
        return EXIT_FAILURE;
    }
    /* Strategy will upload logs every 5 seconds. */
    error = ext_log_upload_strategy_set_upload_timeout(log_upload_strategy_context, 5);
    if (error) {
        return EXIT_FAILURE;
    }
    /* Specify log bucket size constraints. */
    kaa_log_bucket_constraints_t bucket_sizes = {
         .max_bucket_size       = 32,   /* Bucket size in bytes. */
         .max_bucket_log_count  = 1,    /* Maximum log count in one bucket. */
         //.max_bucket_log_count  = 2,    /* Maximum log count in one bucket. */
    };
    /* Initialize the log storage and strategy (by default, they are not set). */
    error = kaa_logging_init(kaa_client_get_context(kaa_client)->log_collector,
        log_storage_context, log_upload_strategy_context, &bucket_sizes);
    if (error) {
        return EXIT_FAILURE;
    }
    
    error = kaa_client_start(kaa_client, example_callback, kaa_client, 1);
    /* Should get here only after Kaa stops. */
    kaa_client_destroy(kaa_client);
    
    if (error) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
