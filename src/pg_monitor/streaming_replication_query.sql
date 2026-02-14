with is_in_recovery as (
  select pg_is_in_recovery() is_replica
)
SELECT
    is_replica
  , case when not is_replica then pg_current_wal_lsn() end master_lsn
  , case when is_replica then pg_last_wal_receive_lsn() end replica_received_lsn
  , case when is_replica then pg_last_wal_replay_lsn() end replica_lsn
  , case when is_replica
      then coalesce((extract(epoch from now() - pg_last_xact_replay_timestamp()) * 1000)::bigint, 0)
      else 0 end replica_delay_ms
from is_in_recovery;
