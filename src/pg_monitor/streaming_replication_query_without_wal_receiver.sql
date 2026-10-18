with is_in_recovery as (
  select pg_is_in_recovery() is_replica
), master_wal as (
  -- Read once, the write position first: a commit visible by then has its
  -- record below the insert position read after it.
  select pg_current_wal_lsn() write_lsn, pg_current_wal_insert_lsn() insert_lsn
  from is_in_recovery
  where not is_replica
)
select
    is_replica
  , (select write_lsn from master_wal) master_lsn
  , case when is_replica then pg_last_wal_receive_lsn() end replica_received_lsn
  , case when is_replica then pg_last_wal_replay_lsn() end replica_lsn
  -- Age of the last replayed commit or abort, NULL if none has been replayed
  -- since the replica started. May be negative when the replica's clock is
  -- behind the master's.
  , case when is_replica
      then (extract(epoch from now() - pg_last_xact_replay_timestamp()) * 1000)::bigint
      end replica_lag_ms
  , null::pg_lsn receiver_latest_end_lsn
  -- Inserted WAL that is not written yet, such as asynchronous commits. At a
  -- page boundary, the insert position is past the page header, at most 40
  -- bytes, without any record.
  , (select insert_lsn - write_lsn > case
        when (write_lsn - '0/0') % current_setting('wal_block_size')::numeric = 0
        then 40 else 0 end
     from master_wal) master_unwritten_wal
  -- Timeline: on a master, that of its WAL; on a replica, the latest one its
  -- control file shows, that of the last restartpoint and of the minimum
  -- recovery point, which may lag behind after a switch. This variant avoids
  -- the WAL receiver statistics the permission error may have come from.
  , case when is_replica
      then greatest(
          (select timeline_id from pg_catalog.pg_control_checkpoint())
        , (select min_recovery_end_timeline from pg_catalog.pg_control_recovery()))
      else (select ('x' || substr(pg_walfile_name(write_lsn), 1, 8))::bit(32)::bigint
            from master_wal)
      end timeline
  , null::boolean receiver_running
  , (extract(epoch from now()) * 1000)::bigint server_now_ms
from is_in_recovery;
