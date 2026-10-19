use super::*;
use std::future::{pending, ready};

#[tokio::test]
async fn saturation_receive_failure_wakes_current_and_future_waiters() {
    for target in [1, RESUME_RECEIVER_LAG, MAX_RECEIVER_LAG] {
        let counter = SaturateCounter::default();
        let rx = counter.expect(target);
        counter.receive(false);
        for rx in [rx, counter.expect(target)] {
            let error = tokio::time::timeout(
                Duration::from_secs(1),
                wait_for(rx, pending(), SATURATE_WAIT_TIMEOUT),
            )
            .await
            .expect("receiver failure left the waiter blocked")
            .unwrap_err();
            assert_eq!(error.to_string(), "saturation receiver failed");
        }
    }
}

#[tokio::test]
async fn saturation_connection_close_wakes_waiter() {
    let counter = SaturateCounter::default();
    let error = tokio::time::timeout(
        Duration::from_secs(1),
        wait_for(
            counter.expect(1),
            ready(quinn::ConnectionError::LocallyClosed),
            SATURATE_WAIT_TIMEOUT,
        ),
    )
    .await
    .expect("closed connection left the waiter blocked")
    .unwrap_err();
    assert!(matches!(
        error.downcast_ref::<quinn::ConnectionError>(),
        Some(quinn::ConnectionError::LocallyClosed)
    ));
}

#[tokio::test]
async fn saturation_stalled_wait_has_a_deadline() {
    let counter = SaturateCounter::default();
    let error = tokio::time::timeout(
        Duration::from_secs(1),
        wait_for(counter.expect(1), pending(), Duration::from_millis(1)),
    )
    .await
    .expect("stalled waiter ignored its deadline")
    .unwrap_err();
    assert_eq!(
        error.downcast_ref::<io::Error>().unwrap().kind(),
        io::ErrorKind::TimedOut
    );
}

#[tokio::test]
async fn saturation_reached_target_completes_current_and_future_waiters() {
    let counter = SaturateCounter::default();
    let rx = counter.expect(1);
    counter.receive(true);
    for rx in [rx, counter.expect(1)] {
        wait_for(rx, pending(), Duration::from_secs(1))
            .await
            .unwrap();
    }
}
