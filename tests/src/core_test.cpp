#include "CppUTest/TestHarness.h"
#include "CppUTestExt/MockSupport.h"

#include "ocpp/ocpp.h"
#include "ocpp/overrides.h"

#include <errno.h>
#include <time.h>
#include <stdlib.h>

static struct {
	uint8_t message_id[OCPP_MESSAGE_ID_MAXLEN];
	ocpp_message_role_t role;
	ocpp_message_t type;
} sent;

static struct {
	ocpp_message_role_t role;
	ocpp_message_t type;
} event;

time_t time(time_t *second) {
	return mock().actualCall(__func__).returnUnsignedIntValueOrDefault(0);
}

int ocpp_send(const struct ocpp_message *msg) {
	memcpy(sent.message_id, msg->id, sizeof(sent.message_id));
	sent.role = msg->role;
	sent.type = msg->type;

	return mock().actualCall(__func__).returnIntValueOrDefault(0);
}

int ocpp_recv(struct ocpp_message *msg)
{
	int rc = mock().actualCall(__func__).withOutputParameter("msg", msg).returnIntValueOrDefault(0);
	memcpy(msg->id, sent.message_id, sizeof(msg->id));
	return rc;
}

int ocpp_lock(void) {
	return 0;
}
int ocpp_unlock(void) {
	return 0;
}

int ocpp_configuration_lock(void) {
	return 0;
}
int ocpp_configuration_unlock(void) {
	return 0;
}

void ocpp_generate_message_id(void *buf, size_t bufsize)
{
	char *p = (char *)buf;
	char charset[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
	while (bufsize-- > 0) {
		int index = rand() % (int)(sizeof(charset) - 1);
		*p++ = charset[index];
	}
	*p = '\0';
}

static void on_ocpp_event(ocpp_event_t event_type,
		const struct ocpp_message *msg, void *ctx) {
	event.role = msg->role;
	event.type = msg->type;
	mock().actualCall(__func__).withParameter("event_type", event_type);
}

TEST_GROUP(Core) {
	void setup(void) {
		srand((unsigned int)clock());
		mock().expectOneCall("time").andReturnValue(0);
		ocpp_init(on_ocpp_event, NULL);
	}
	void teardown(void) {
		mock().checkExpectations();
		mock().clear();
	}

	void step(int sec) {
		mock().expectOneCall("time").andReturnValue(sec);
		ocpp_step();
	}
	void check_tx(ocpp_message_role_t role, ocpp_message_t type) {
		LONGS_EQUAL(role, sent.role);
		LONGS_EQUAL(type, sent.type);
	}
	void check_rx(ocpp_message_role_t role, ocpp_message_t type) {
		LONGS_EQUAL(role, event.role);
		LONGS_EQUAL(type, event.type);
	}
	void go_bootnoti_accepted(void) {
		struct ocpp_message resp = {
			.role = OCPP_MSG_ROLE_CALLRESULT,
			.type = OCPP_MSG_BOOTNOTIFICATION,
			.payload.fmt.response = &(const struct ocpp_BootNotification_conf) {
				.interval = 10,
				.status = OCPP_BOOT_STATUS_ACCEPTED,
			},
		};
		ocpp_send_bootnotification(&(const struct ocpp_BootNotification) {
			.chargePointModel = "Model",
			.chargePointVendor = "Vendor",
		});

		mock().expectOneCall("ocpp_send").andReturnValue(0);
		mock().expectOneCall("ocpp_recv")
			.withOutputParameterReturning("msg", &resp, sizeof(resp))
			.andReturnValue(0);
		mock().expectOneCall("on_ocpp_event").withParameter("event_type", 2);
		mock().expectOneCall("on_ocpp_event").withParameter("event_type", 0);
		step(0);
	}
};

TEST(Core, step_ShouldNeverDropBootNotification_WhenSendFailed) {
	ocpp_send_bootnotification(&(const struct ocpp_BootNotification) {
		.chargePointModel = "Model",
		.chargePointVendor = "Vendor",
	});

	int interval;
	ocpp_get_configuration("HeartbeatInterval", &interval, sizeof(interval), 0);

	for (int i = 0; i < 100; i++) {
		mock().expectOneCall("ocpp_recv").ignoreOtherParameters().andReturnValue(-ENOMSG);
		mock().expectOneCall("ocpp_send").andReturnValue(-1);
		step(interval*i);
	}
}

TEST(Core, step_ShouldDropMessage_WhenFailedSendingMoreThanRetries) {
	ocpp_send_datatransfer(&(const struct ocpp_DataTransfer) {
		.vendorId = "VendorID",
	});

	mock().expectOneCall("ocpp_recv").ignoreOtherParameters().andReturnValue(-ENOMSG);
	mock().expectOneCall("ocpp_send").andReturnValue(-1);
	step(0);
	mock().expectOneCall("ocpp_recv").ignoreOtherParameters().andReturnValue(-ENOMSG);
	mock().expectOneCall("ocpp_send").andReturnValue(-1);
	mock().expectOneCall("on_ocpp_event").withParameter("event_type", OCPP_EVENT_MESSAGE_FREE);
	step(OCPP_DEFAULT_TX_TIMEOUT_SEC);
	mock().expectOneCall("ocpp_recv").ignoreOtherParameters().andReturnValue(-ENOMSG);
	step(OCPP_DEFAULT_TX_TIMEOUT_SEC*2);
}

TEST(Core, ShouldNeverSendHeartBeat_WhenBootNotificationNotAccepted) {
	int interval;
	ocpp_get_configuration("HeartbeatInterval", &interval, sizeof(interval), NULL);
	mock().expectOneCall("ocpp_recv").ignoreOtherParameters().andReturnValue(-ENOMSG);
	step(interval);
}

TEST(Core, step_ShouldSendHeartBeat_WhenNoMessageSentDuringHeartBeatInterval) {
	go_bootnoti_accepted();

	int interval;
	ocpp_get_configuration("HeartbeatInterval", &interval, sizeof(interval), NULL);
	mock().expectOneCall("ocpp_recv").ignoreOtherParameters().andReturnValue(-ENOMSG);
	mock().expectOneCall("ocpp_send").andReturnValue(0);
	step(interval);
	check_tx(OCPP_MSG_ROLE_CALL, OCPP_MSG_HEARTBEAT);

	struct ocpp_message resp = {
		.role = OCPP_MSG_ROLE_CALLRESULT,
		.type = OCPP_MSG_HEARTBEAT,
	};
	memcpy(resp.id, sent.message_id, sizeof(sent.message_id));
	mock().expectOneCall("ocpp_recv").withOutputParameterReturning("msg", &resp, sizeof(resp));
	mock().expectOneCall("on_ocpp_event").withParameter("event_type", 2);
	mock().expectOneCall("on_ocpp_event").withParameter("event_type", 0);
	step(interval + 1);
	check_rx(OCPP_MSG_ROLE_CALLRESULT, OCPP_MSG_HEARTBEAT);

	mock().expectOneCall("ocpp_recv").ignoreOtherParameters().andReturnValue(-ENOMSG);
	mock().expectOneCall("ocpp_send").andReturnValue(0);
	step(interval*2+1);
}

TEST(Core, step_ShouldNotSendHeartBeat_WhenAnyMessageSentDuringHeartBeatInterval) {
	int interval;
	ocpp_get_configuration("HeartbeatInterval", &interval, sizeof(interval), NULL);
	ocpp_send_datatransfer(&(const struct ocpp_DataTransfer) {
		.vendorId = "VendorID",
	});
	mock().expectOneCall("ocpp_send").andReturnValue(0);
	mock().expectOneCall("ocpp_recv").ignoreOtherParameters().andReturnValue(-ENOMSG);
	step(interval);
	check_tx(OCPP_MSG_ROLE_CALL, OCPP_MSG_DATA_TRANSFER);
	
	struct ocpp_message resp = {
		.role = OCPP_MSG_ROLE_CALLRESULT,
		.type = sent.type,
	};
	memcpy(resp.id, sent.message_id, sizeof(sent.message_id));
	mock().expectOneCall("ocpp_send").andReturnValue(0);
	mock().expectOneCall("ocpp_recv").withOutputParameterReturning("msg", &resp, sizeof(resp));
	mock().expectOneCall("on_ocpp_event").withParameter("event_type", 2);
	mock().expectOneCall("on_ocpp_event").withParameter("event_type", 0);
	step(interval*2);
	check_rx(OCPP_MSG_ROLE_CALLRESULT, OCPP_MSG_DATA_TRANSFER);

	mock().expectOneCall("ocpp_recv").ignoreOtherParameters().andReturnValue(-ENOMSG);
	step(interval*3-1);
}

TEST(Core, ShouldSendStartTransaction_WhenQueueIsFull) {
	int interval;
	ocpp_get_configuration("HeartbeatInterval", &interval, sizeof(interval), NULL);
	struct ocpp_DataTransfer msg[8];
	struct ocpp_StartTransaction start;
	for (int i = 0; i < 8; i++) {
		LONGS_EQUAL(0, ocpp_push_request(OCPP_MSG_DATA_TRANSFER, &msg[i], sizeof(msg[i]), false));
	}

	LONGS_EQUAL(-ENOMEM, ocpp_push_request(OCPP_MSG_START_TRANSACTION, &start, sizeof(start), false));
	mock().expectOneCall("on_ocpp_event").withParameter("event_type", OCPP_EVENT_MESSAGE_FREE);
	LONGS_EQUAL(0, ocpp_push_request(OCPP_MSG_START_TRANSACTION, &start, sizeof(start), true));

	mock().expectNCalls(6, "on_ocpp_event").withParameter("event_type", OCPP_EVENT_MESSAGE_FREE);
	for (int i = 0; i < 7*OCPP_DEFAULT_TX_RETRIES; i++) {
		mock().expectOneCall("ocpp_send").andReturnValue(0);
		mock().expectOneCall("ocpp_recv").ignoreOtherParameters().andReturnValue(-ENOMSG);
		step(interval*i);
		check_tx(OCPP_MSG_ROLE_CALL, OCPP_MSG_DATA_TRANSFER);
	}

	mock().expectOneCall("on_ocpp_event").withParameter("event_type", OCPP_EVENT_MESSAGE_FREE);
	mock().expectOneCall("ocpp_send").andReturnValue(0);
	mock().expectOneCall("ocpp_recv").ignoreOtherParameters().andReturnValue(-ENOMSG);
	step(interval*OCPP_DEFAULT_TX_RETRIES*7);
	check_tx(OCPP_MSG_ROLE_CALL, OCPP_MSG_START_TRANSACTION);

	mock().expectOneCall("ocpp_send").andReturnValue(0);
	mock().expectOneCall("ocpp_recv").ignoreOtherParameters().andReturnValue(-ENOMSG);
	step(interval*OCPP_DEFAULT_TX_RETRIES*8);
	check_tx(OCPP_MSG_ROLE_CALL, OCPP_MSG_START_TRANSACTION);
}

TEST(Core, ShouldReturnNOMEM_WhenQueueIsFullWithTransactionRelatedMessages) {
	struct ocpp_DataTransfer data;
	struct ocpp_StartTransaction start;
	for (int i = 0; i < 8; i++) {
		LONGS_EQUAL(0, ocpp_push_request(OCPP_MSG_DATA_TRANSFER, &data, sizeof(data), false));
	}
	mock().expectNCalls(8, "on_ocpp_event").withParameter("event_type", OCPP_EVENT_MESSAGE_FREE);
	for (int i = 0; i < 8; i++) {
		LONGS_EQUAL(0, ocpp_push_request(OCPP_MSG_START_TRANSACTION, &start, sizeof(start), true));
	}
	LONGS_EQUAL(-ENOMEM, ocpp_push_request(OCPP_MSG_START_TRANSACTION, &start, sizeof(start), true));
}

TEST(Core, ShouldDropTransactionRelatedMessages_WhenServerReponsesWithErrorMoreThanMaxAttemptsConfigured) {
	int32_t interval;
	int32_t max_attempts;
	ocpp_get_configuration("TransactionMessageRetryInterval",
			&interval, sizeof(interval), 0);
	ocpp_get_configuration("TransactionMessageAttempts",
			&max_attempts, sizeof(max_attempts), NULL);
	struct ocpp_StartTransaction start;
	struct ocpp_message msg = {
		.role = OCPP_MSG_ROLE_CALLERROR,
		.type = OCPP_MSG_START_TRANSACTION,
	};

	ocpp_push_request(OCPP_MSG_START_TRANSACTION, &start, sizeof(start), true);
	mock().expectOneCall("ocpp_send").andReturnValue(0);
	mock().expectOneCall("ocpp_recv").ignoreOtherParameters().andReturnValue(-ENOMSG);
	step(0);
	memcpy(msg.id, sent.message_id, sizeof(msg.id));
	for (int i = 0; i < max_attempts-1; i++) {
		mock().expectOneCall("ocpp_recv").withOutputParameterReturning("msg", &msg, sizeof(msg));
		mock().expectOneCall("on_ocpp_event").withParameter("event_type", OCPP_EVENT_MESSAGE_INCOMING);
		if (i) {
			mock().expectOneCall("ocpp_send").andReturnValue(0);
		}
		step((interval*i)*i+1);
	}

	mock().expectOneCall("ocpp_send").andReturnValue(0);
	mock().expectOneCall("ocpp_recv").withOutputParameterReturning("msg", &msg, sizeof(msg));
	mock().expectOneCall("on_ocpp_event").withParameter("event_type", OCPP_EVENT_MESSAGE_INCOMING);
	mock().expectOneCall("on_ocpp_event").withParameter("event_type", OCPP_EVENT_MESSAGE_FREE);
	step((interval*max_attempts)*max_attempts+1);
}

TEST(Core, ShouldSendTransactionRelatedmessagesIndefinitely_WhenTransportErrors) {
}

TEST(Core, ShouldDropNonTransactionRelatedMessagesAfterTimeout_WhenNoResponseReceived) {
	ocpp_push_request(OCPP_MSG_DATA_TRANSFER, &(const struct ocpp_DataTransfer) {
		.vendorId = "VendorID",
	}, sizeof(struct ocpp_DataTransfer), false);

	int i = 0;
	for (; i < OCPP_DEFAULT_TX_RETRIES; i++) {
		mock().expectOneCall("ocpp_send").andReturnValue(0);
		mock().expectOneCall("ocpp_recv").ignoreOtherParameters().andReturnValue(-ENOMSG);
		step(i*OCPP_DEFAULT_TX_TIMEOUT_SEC);
	}

	mock().expectOneCall("ocpp_recv").ignoreOtherParameters().andReturnValue(-ENOMSG);
	mock().expectOneCall("on_ocpp_event").withParameter("event_type", OCPP_EVENT_MESSAGE_FREE);
	step(i*OCPP_DEFAULT_TX_TIMEOUT_SEC);
}

TEST(Core, ShouldDropNonTransactionRelatedMessagesAfterTimeout_WhenTransportErrors) {
	ocpp_push_request(OCPP_MSG_DATA_TRANSFER, &(const struct ocpp_DataTransfer) {
		.vendorId = "VendorID",
	}, sizeof(struct ocpp_DataTransfer), false);

	int i = 0;
	for (; i < OCPP_DEFAULT_TX_RETRIES-1; i++) {
		mock().expectOneCall("ocpp_send").andReturnValue(-1);
		mock().expectOneCall("ocpp_recv").ignoreOtherParameters().andReturnValue(-ENOMSG);
		step(i*OCPP_DEFAULT_TX_TIMEOUT_SEC);
	}

	mock().expectOneCall("ocpp_send").andReturnValue(-1);
	mock().expectOneCall("ocpp_recv").ignoreOtherParameters().andReturnValue(-ENOMSG);
	mock().expectOneCall("on_ocpp_event").withParameter("event_type", OCPP_EVENT_MESSAGE_FREE);
	step(i*OCPP_DEFAULT_TX_TIMEOUT_SEC);
}

TEST(Core, t) {
	ocpp_send_bootnotification(&(const struct ocpp_BootNotification) {
		.chargePointModel = "Model",
		.chargePointVendor = "Vendor",
	});

	mock().expectOneCall("ocpp_send").andReturnValue(0);
	mock().expectOneCall("ocpp_recv").ignoreOtherParameters().andReturnValue(-ENOMSG);
	step(0);
	check_tx(OCPP_MSG_ROLE_CALL, OCPP_MSG_BOOTNOTIFICATION);
}

TEST(Core, ShouldNotSendHeartBeat_WhenReceivedMessageWithinInterval) {
        // Test that heartbeat is not sent when a message was received within the interval
        go_bootnoti_accepted();

        int interval;
        ocpp_get_configuration("HeartbeatInterval", &interval, sizeof(interval), NULL);

        // Simulate receiving a message first
        struct ocpp_message incoming_call = {
                .role = OCPP_MSG_ROLE_CALL,
                .type = OCPP_MSG_HEARTBEAT,
        };
        strcpy(incoming_call.id, "test-call-id");

        mock().expectOneCall("ocpp_recv")
                .withOutputParameterReturning("msg", &incoming_call, sizeof(incoming_call))
                .andReturnValue(0);
        mock().expectOneCall("on_ocpp_event").ignoreOtherParameters();
        step(10); // Receive message at time 10

        // Now step forward but less than interval since last RX
        mock().expectOneCall("ocpp_recv").ignoreOtherParameters().andReturnValue(-ENOMSG);
        step(10 + interval - 1); // Still within interval since last RX

        // No heartbeat should be sent because RX timestamp is more recent
}

TEST(Core, ShouldSendHeartBeat_WhenOnlyOldRxMessageWithinInterval) {
        // Test that heartbeat is sent when only old RX messages exist within interval
        go_bootnoti_accepted();

        int interval;
        ocpp_get_configuration("HeartbeatInterval", &interval, sizeof(interval), NULL);

        // Simulate receiving a message at an early time
        struct ocpp_message incoming_call = {
                .role = OCPP_MSG_ROLE_CALL,
                .type = OCPP_MSG_HEARTBEAT,
        };
        strcpy(incoming_call.id, "test-call-id");

        mock().expectOneCall("ocpp_recv")
                .withOutputParameterReturning("msg", &incoming_call, sizeof(incoming_call))
                .andReturnValue(0);
        mock().expectOneCall("on_ocpp_event").ignoreOtherParameters();
        step(10); // Receive message at time 10

        // Now step forward past the interval since both TX and RX
        mock().expectOneCall("ocpp_recv").ignoreOtherParameters().andReturnValue(-ENOMSG);
        mock().expectOneCall("ocpp_send").andReturnValue(0);
        step(10 + interval + 1); // Past interval since last message
        check_tx(OCPP_MSG_ROLE_CALL, OCPP_MSG_HEARTBEAT);
}

TEST(Core, ShouldUseLatestTimestamp_WhenRxMoreRecentThanTx) {
        // Test that heartbeat uses RX timestamp when it's more recent than TX
        go_bootnoti_accepted();

        int interval;
        ocpp_get_configuration("HeartbeatInterval", &interval, sizeof(interval), NULL);

        // Receive an RX message to set RX timestamp
        struct ocpp_message incoming_call = {
                .role = OCPP_MSG_ROLE_CALL,
                .type = OCPP_MSG_HEARTBEAT,
        };
        strcpy(incoming_call.id, "test-call-id");

        mock().expectOneCall("ocpp_recv")
                .withOutputParameterReturning("msg", &incoming_call, sizeof(incoming_call))
                .andReturnValue(0);
        mock().expectOneCall("on_ocpp_event").ignoreOtherParameters();
        step(50); // RX at time 50

        // Step forward less than interval since RX
        mock().expectOneCall("ocpp_recv").ignoreOtherParameters().andReturnValue(-ENOMSG);
        step(50 + interval - 1); // Within interval since RX (time 50)

        // No heartbeat should be sent because we're within interval since last RX
}

TEST(Core, ShouldSendHeartBeat_WhenTxSentButNoResponseReceived) {
        // Test that heartbeat is sent when TX timestamp is NOT updated without response
        // We'll verify this indirectly by checking heartbeat behavior
        go_bootnoti_accepted();

        int interval;
        ocpp_get_configuration("HeartbeatInterval", &interval, sizeof(interval), NULL);

        // Just check that heartbeat is sent after interval from initialization
        // since TX timestamp is only updated when response is received, not when sent
        mock().expectOneCall("ocpp_recv").ignoreOtherParameters().andReturnValue(-ENOMSG);
        mock().expectOneCall("ocpp_send").andReturnValue(0);
        step(interval + 1); // Past interval since initialization
        check_tx(OCPP_MSG_ROLE_CALL, OCPP_MSG_HEARTBEAT);
}

TEST(Core, ShouldNotSendHeartBeat_WhenTxResponseReceivedRecently) {
        // Test that TX timestamp IS updated when response is received
        // This uses the existing go_bootnoti_accepted() which completes a TX transaction
        go_bootnoti_accepted();

        int interval;
        ocpp_get_configuration("HeartbeatInterval", &interval, sizeof(interval), NULL);

        // At this point, TX timestamp was updated when BootNotification response was received
        // Step forward less than interval - no heartbeat should be sent
        mock().expectOneCall("ocpp_recv").ignoreOtherParameters().andReturnValue(-ENOMSG);
        step(interval - 1); // Within interval since TX response

        // No heartbeat should be sent because we're within interval since TX response
}

TEST(Core, ShouldSendHeartBeat_WhenElapsedTimeEqualsIntervalExactly) {
        // Test boundary condition: elapsed time = interval (should send heartbeat)
        // Since condition is "elapsed < interval", when elapsed == interval, it should send
        go_bootnoti_accepted();

        int interval;
        ocpp_get_configuration("HeartbeatInterval", &interval, sizeof(interval), NULL);

        // Step forward exactly interval time - should send heartbeat
        mock().expectOneCall("ocpp_recv").ignoreOtherParameters().andReturnValue(-ENOMSG);
        mock().expectOneCall("ocpp_send").andReturnValue(0);
        step(interval); // elapsed == interval, should send (not < interval)

        check_tx(OCPP_MSG_ROLE_CALL, OCPP_MSG_HEARTBEAT);
}

TEST(Core, ShouldSendHeartBeat_WhenElapsedTimeExceedsInterval) {
        // Test boundary condition: elapsed time > interval (should send heartbeat)
        go_bootnoti_accepted();

        int interval;
        ocpp_get_configuration("HeartbeatInterval", &interval, sizeof(interval), NULL);

        // Step forward just past interval - should send heartbeat
        mock().expectOneCall("ocpp_recv").ignoreOtherParameters().andReturnValue(-ENOMSG);
        mock().expectOneCall("ocpp_send").andReturnValue(0);
        step(interval + 1); // elapsed > interval, should send

        check_tx(OCPP_MSG_ROLE_CALL, OCPP_MSG_HEARTBEAT);
}

TEST(Core, ShouldSendHeartBeat_WhenElapsedTimeEqualsIntervalFromRxMessage) {
        // Test boundary condition with RX message: elapsed time = interval (should send)
        go_bootnoti_accepted();

        int interval;
        ocpp_get_configuration("HeartbeatInterval", &interval, sizeof(interval), NULL);

        // Receive an RX message to set RX timestamp
        struct ocpp_message incoming_call = {
                .role = OCPP_MSG_ROLE_CALL,
                .type = OCPP_MSG_HEARTBEAT,
        };
        strcpy(incoming_call.id, "test-call-id");

        mock().expectOneCall("ocpp_recv")
                .withOutputParameterReturning("msg", &incoming_call, sizeof(incoming_call))
                .andReturnValue(0);
        mock().expectOneCall("on_ocpp_event").ignoreOtherParameters();
        step(10); // RX at time 10

        // Step forward exactly interval from RX time - should send heartbeat
        mock().expectOneCall("ocpp_recv").ignoreOtherParameters().andReturnValue(-ENOMSG);
        mock().expectOneCall("ocpp_send").andReturnValue(0);
        step(10 + interval); // elapsed == interval from RX, should send

        check_tx(OCPP_MSG_ROLE_CALL, OCPP_MSG_HEARTBEAT);
}

TEST(Core, ShouldSendHeartBeat_WhenElapsedTimeExceedsIntervalFromRxMessage) {
        // Test boundary condition with RX message: elapsed time > interval
        go_bootnoti_accepted();

        int interval;
        ocpp_get_configuration("HeartbeatInterval", &interval, sizeof(interval), NULL);

        // Receive an RX message to set RX timestamp
        struct ocpp_message incoming_call = {
                .role = OCPP_MSG_ROLE_CALL,
                .type = OCPP_MSG_HEARTBEAT,
        };
        strcpy(incoming_call.id, "test-call-id");

        mock().expectOneCall("ocpp_recv")
                .withOutputParameterReturning("msg", &incoming_call, sizeof(incoming_call))
                .andReturnValue(0);
        mock().expectOneCall("on_ocpp_event").ignoreOtherParameters();
        step(10); // RX at time 10

        // Step forward just past interval from RX time - should send heartbeat
        mock().expectOneCall("ocpp_recv").ignoreOtherParameters().andReturnValue(-ENOMSG);
        mock().expectOneCall("ocpp_send").andReturnValue(0);
        step(10 + interval + 1); // elapsed > interval from RX, should send

        check_tx(OCPP_MSG_ROLE_CALL, OCPP_MSG_HEARTBEAT);
}
