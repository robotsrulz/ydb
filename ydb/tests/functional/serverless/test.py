# -*- coding: utf-8 -*-
import logging
import os
import time
import functools

import pytest
from hamcrest import (
    assert_that,
    contains_inanyorder,
    not_none,
)

import ydb

from tornado import gen
from tornado.ioloop import IOLoop

logger = logging.getLogger(__name__)


def test_fixtures(ydb_hostel_db, ydb_serverless_db):
    logger.debug(
        "test for serverless db %s over hostel db %s", ydb_serverless_db, ydb_hostel_db
    )


def test_create_table(ydb_hostel_db, ydb_serverless_db, ydb_endpoint, metering_file_path, ydb_private_client):
    logger.debug(
        "test for serverless db %s over hostel db %s", ydb_serverless_db, ydb_hostel_db
    )

    database = ydb_serverless_db

    driver_config = ydb.DriverConfig(
        ydb_endpoint,
        database
    )
    logger.info(" database is %s", database)

    driver = ydb.Driver(driver_config)
    driver.wait(120)

    driver.scheme_client.make_directory(os.path.join(database, "dirA0"))

    driver.scheme_client.make_directory(os.path.join(database, "dirA1"))
    driver.scheme_client.make_directory(os.path.join(database, "dirA1", "dirB1"))

    with ydb.SessionPool(driver) as pool:
        def create_table(session, path):
            session.create_table(
                path,
                ydb.TableDescription()
                .with_column(ydb.Column('id', ydb.OptionalType(ydb.DataType.Uint64)))
                .with_column(ydb.Column('value_string', ydb.OptionalType(ydb.DataType.Utf8)))
                .with_column(ydb.Column('value_num', ydb.OptionalType(ydb.DataType.Uint64)))
                .with_primary_key('id')
            )

        pool.retry_operation_sync(create_table, None, os.path.join(database, "dirA1", "dirB1", "table"))
        pool.retry_operation_sync(create_table, None, os.path.join(database, "dirA1", "dirB1", "table1"))

        def write_some_data(session, path):
            session.transaction().execute(
                """
                PRAGMA Kikimr.UseNewEngine="True";
                UPSERT INTO `{}` (id, value_string, value_num)
                           VALUES (1u, "Ok", 0u),
                                  (2u, "Also_Ok", 0u),
                                  (3u, "And_Ok_With_Locks", 0u);
                                  """.format(path),
                commit_tx=True)

        pool.retry_operation_sync(write_some_data, None, os.path.join(database, "dirA1", "dirB1", "table"))

        def drop_table(session, path):
            session.drop_table(
                path
            )

        pool.retry_operation_sync(drop_table, None, os.path.join(database, "dirA1", "dirB1", "table"))


def test_turn_on_serverless_storage_billing(ydb_hostel_db, ydb_serverless_db, ydb_endpoint, metering_file_path, ydb_private_client):
    logger.debug(
        "test for serverless db %s over hostel db %s", ydb_serverless_db, ydb_hostel_db
    )

    database = ydb_serverless_db

    driver_config = ydb.DriverConfig(
        ydb_endpoint,
        database
    )
    logger.info(" database is %s", database)

    driver = ydb.Driver(driver_config)
    driver.wait(120)

    driver.scheme_client.make_directory(os.path.join(database, "dirA0"))

    driver.scheme_client.make_directory(os.path.join(database, "dirA1"))
    driver.scheme_client.make_directory(os.path.join(database, "dirA1", "dirB1"))

    with ydb.SessionPool(driver) as pool:
        def create_table(session, path):
            session.create_table(
                path,
                ydb.TableDescription()
                .with_column(ydb.Column('id', ydb.OptionalType(ydb.DataType.Uint64)))
                .with_column(ydb.Column('value_string', ydb.OptionalType(ydb.DataType.Utf8)))
                .with_column(ydb.Column('value_num', ydb.OptionalType(ydb.DataType.Uint64)))
                .with_primary_key('id')
            )

        pool.retry_operation_sync(create_table, None, os.path.join(database, "dirA1", "dirB1", "table"))
        pool.retry_operation_sync(create_table, None, os.path.join(database, "dirA1", "dirB1", "table1"))

        def write_some_data(session, path):
            session.transaction().execute(
                """
                PRAGMA Kikimr.UseNewEngine="True";
                UPSERT INTO `{}` (id, value_string, value_num)
                           VALUES (1u, "Ok", 0u),
                                  (2u, "Also_Ok", 0u),
                                  (3u, "And_Ok_With_Locks", 0u);
                                  """.format(path),
                commit_tx=True)

        pool.retry_operation_sync(write_some_data, None, os.path.join(database, "dirA1", "dirB1", "table"))

        ydb_private_client.add_config_item("FeatureFlags { AllowServerlessStorageBillingForSchemeShard: true }")
        while True:
            with open(metering_file_path, 'r') as metering_file:
                lines = metering_file.readlines()
                if lines:
                    logger.info(" metering has data %s", lines[-1])
                    break
                logger.info(" wait data in metering file %s", metering_file_path)
                time.sleep(15)

        def drop_table(session, path):
            session.drop_table(
                path
            )

        pool.retry_operation_sync(drop_table, None, os.path.join(database, "dirA1", "dirB1", "table"))


def test_create_table_with_quotas(ydb_hostel_db, ydb_quoted_serverless_db, ydb_endpoint, ydb_cluster):
    logger.debug(
        "test for serverless db %s over hostel db %s", ydb_quoted_serverless_db, ydb_hostel_db
    )

    database = ydb_quoted_serverless_db

    driver_config = ydb.DriverConfig(
        ydb_endpoint,
        database
    )
    logger.info(" database is %s", database)

    driver = ydb.Driver(driver_config)
    driver.wait(120)

    driver.scheme_client.make_directory(os.path.join(database, "dirA0"))

    def create_table(session, path):
        logger.debug("creating table %s", path)
        session.create_table(
            path,
            ydb.TableDescription()
            .with_column(ydb.Column('id', ydb.OptionalType(ydb.DataType.Uint64)))
            .with_column(ydb.Column('value_string', ydb.OptionalType(ydb.DataType.Utf8)))
            .with_column(ydb.Column('value_num', ydb.OptionalType(ydb.DataType.Uint64)))
            .with_primary_key('id')
        )

    with ydb.SessionPool(driver) as pool:
        pool.retry_operation_sync(create_table, None, os.path.join(database, "dirA0", "table"))

        with pool.checkout() as session:
            # We made two operations (1 mkdir, 1 table) so we should be out of per-minute quota
            with pytest.raises(ydb.Overloaded) as excinfo:
                create_table(session, os.path.join(database, "dirA0", "table2"))
                create_table(session, os.path.join(database, "dirA0", "table3"))
            assert "exceeded a limit" in str(excinfo.value)


def test_create_table_with_alter_quotas(ydb_hostel_db, ydb_serverless_db, ydb_endpoint, ydb_cluster):
    logger.debug(
        "test for serverless db %s over hostel db %s", ydb_serverless_db, ydb_hostel_db
    )

    database = ydb_serverless_db

    status = ydb_cluster.get_database_status(database)
    assert len(status.schema_operation_quotas.leaky_bucket_quotas) == 0

    logger.debug("adding schema quotas to db %s", database)
    ydb_cluster.alter_serverless_database(database, schema_quotas=((2, 60), (4, 600)))

    status = ydb_cluster.get_database_status(database)
    assert len(status.schema_operation_quotas.leaky_bucket_quotas) == 2

    driver_config = ydb.DriverConfig(
        ydb_endpoint,
        database
    )
    logger.info(" database is %s", database)

    driver = ydb.Driver(driver_config)
    driver.wait(120)

    driver.scheme_client.make_directory(os.path.join(database, "dirA0"))

    def create_table(session, path):
        logger.debug("creating table %s", path)
        session.create_table(
            path,
            ydb.TableDescription()
            .with_column(ydb.Column('id', ydb.OptionalType(ydb.DataType.Uint64)))
            .with_column(ydb.Column('value_string', ydb.OptionalType(ydb.DataType.Utf8)))
            .with_column(ydb.Column('value_num', ydb.OptionalType(ydb.DataType.Uint64)))
            .with_primary_key('id')
        )

    with ydb.SessionPool(driver) as pool:
        pool.retry_operation_sync(create_table, None, os.path.join(database, "dirA0", "table"))

        with pool.checkout() as session:
            # We made two operations (1 mkdir, 1 table) so we should be out of per-minute quota
            with pytest.raises(ydb.Overloaded) as excinfo:
                create_table(session, os.path.join(database, "dirA0", "table2"))
                create_table(session, os.path.join(database, "dirA0", "table3"))
            assert "exceeded a limit" in str(excinfo.value)


def test_database_with_disk_quotas(ydb_hostel_db, ydb_disk_quoted_serverless_db, ydb_endpoint, ydb_cluster):
    logger.debug(
        "test for serverless db %s over hostel db %s", ydb_disk_quoted_serverless_db, ydb_hostel_db
    )

    database = ydb_disk_quoted_serverless_db

    driver_config = ydb.DriverConfig(
        ydb_endpoint,
        database
    )
    logger.info(" database is %s", database)

    driver = ydb.Driver(driver_config)
    driver.wait(120)

    def create_table(session, path):
        logger.debug("creating table %s", path)
        session.create_table(
            path,
            ydb.TableDescription()
            .with_column(ydb.Column('id', ydb.OptionalType(ydb.DataType.Uint64)))
            .with_column(ydb.Column('value_string', ydb.OptionalType(ydb.DataType.Utf8)))
            .with_column(ydb.Column('value_num', ydb.OptionalType(ydb.DataType.Uint64)))
            .with_primary_key('id')
        )

    sessions = []

    class SessionHolder(object):
        def __init__(self, session):
            self.session = session
        def __enter__(self):
            return self.session
        def __exit__(self, exc_type=None, exc_value=None, exc_traceback=None):
            if exc_type is None and exc_value is None:
                sessions.append(self.session)
            else:
                self.session.reset()

    @gen.coroutine
    def async_session():
        if sessions:
            session = sessions.pop()
        else:
            session = yield driver.table_client.session().async_create()
        raise gen.Return(SessionHolder(session))

    def restart_coro_on_bad_session(func):
        @gen.coroutine
        @functools.wraps(func)
        def wrapped(*args, **kwargs):
            while True:
                try:
                    res = yield func(*args, **kwargs)
                except ydb.BadSession:
                    pass
                else:
                    break
            raise gen.Return(res)
        return wrapped

    @restart_coro_on_bad_session
    @gen.coroutine
    def async_write_key(path, key, value, ignore_out_of_space=True):
        try:
            with (yield async_session()) as session:
                query = yield session.async_prepare('''\
                    PRAGMA Kikimr.UseNewEngine="True";
                    DECLARE $key AS Uint64;
                    DECLARE $value AS Utf8;

                    UPSERT INTO `{path}` (id, value_string) VALUES ($key, $value);
                '''.format(path=path))
                with session.transaction(ydb.SerializableReadWrite()) as tx:
                    yield tx.async_execute(
                        query,
                        parameters={
                            '$key': key,
                            '$value': value,
                        },
                        commit_tx=True,
                    )
        except ydb.Unavailable as e:
            if not ignore_out_of_space or 'OUT_OF_SPACE' not in str(e):
                raise

    @restart_coro_on_bad_session
    @gen.coroutine
    def async_erase_key(path, key):
        with (yield async_session()) as session:
            query = yield session.async_prepare('''\
                PRAGMA Kikimr.UseNewEngine="True";
                DECLARE $key AS Uint64;

                DELETE FROM `{path}` WHERE id = $key;
            '''.format(path=path))
            with session.transaction(ydb.SerializableReadWrite()) as tx:
                logger.debug("erasing table %s key %r", path, key)
                yield tx.async_execute(
                    query,
                    parameters={
                        '$key': key,
                    },
                    commit_tx=True,
                )

    @gen.coroutine
    def async_write_keys(path, start, cnt):
        futures = []
        for i in range(start, start + cnt):
            futures.append(async_write_key(path, i, 'a' * 71680))
        waiter = gen.WaitIterator(*futures)
        while not waiter.done():
            yield waiter.next()

    @gen.coroutine
    def async_erase_keys(path, start, cnt):
        futures = []
        for i in range(start, start + cnt):
            futures.append(async_erase_key(path, i))
        waiter = gen.WaitIterator(*futures)
        while not waiter.done():
            yield waiter.next()

    class BulkUpsertRow(object):
        __slots__ = ('id', 'value_string')

        def __init__(self, id, value_string):
            self.id = id
            self.value_string = value_string

    @gen.coroutine
    def async_bulk_upsert(path, rows):
        column_types = ydb.BulkUpsertColumns() \
            .add_column('id', ydb.OptionalType(ydb.PrimitiveType.Uint64)) \
            .add_column('value_string', ydb.OptionalType(ydb.PrimitiveType.Utf8))
        yield driver.table_client.async_bulk_upsert(path, rows, column_types)

    driver.scheme_client.make_directory(os.path.join(database, "dirA0"))
    with ydb.SessionPool(driver) as pool:
        path = os.path.join(database, "dirA0", "table")
        pool.retry_operation_sync(create_table, None, path)

        for start in range(0, 1000, 100):
            IOLoop.current().run_sync(lambda: async_write_keys(path, start=start, cnt=100))

        for _ in range(30):
            time.sleep(1)
            described = ydb_cluster.client.describe(database, '')
            logger.debug('database state after write_keys: %s', described)
            if described.PathDescription.DomainDescription.DomainState.DiskQuotaExceeded:
                break
        else:
            assert False, 'database did not move into DiskQuotaExceeded state'

        # Writes should be denied when database moves into DiskQuotaExceeded state
        time.sleep(1)
        with pytest.raises(ydb.Unavailable, match=r'.*OUT_OF_SPACE.*'):
            IOLoop.current().run_sync(lambda: async_write_key(path, 0, 'test', ignore_out_of_space=False))
        with pytest.raises(ydb.Unavailable, match=r'.*out of disk space.*'):
            IOLoop.current().run_sync(lambda: async_bulk_upsert(path, [BulkUpsertRow(0, 'test')]))

        for start in range(0, 1000, 100):
            IOLoop.current().run_sync(lambda: async_erase_keys(path, start=start, cnt=100))

        for _ in range(30):
            time.sleep(1)
            described = ydb_cluster.client.describe(database, '')
            logger.debug('database state after erase_keys: %s', described)
            if not described.PathDescription.DomainDescription.DomainState.DiskQuotaExceeded:
                break
        else:
            assert False, 'database did not move out of DiskQuotaExceeded state'

        # Writes should be allowed again when database moves out of DiskQuotaExceeded state
        time.sleep(1)
        IOLoop.current().run_sync(lambda: async_write_key(path, 0, 'test', ignore_out_of_space=False))


def test_discovery(ydb_hostel_db, ydb_serverless_db, ydb_endpoint):
    def list_endpoints(database):
        logger.debug("List endpoints of %s", database)
        resolver = ydb.DiscoveryEndpointsResolver(ydb.DriverConfig(ydb_endpoint, database))
        result = resolver.resolve()
        if result is not None:
            return result.endpoints
        return result

    hostel_db_endpoints = list_endpoints(ydb_hostel_db)
    serverless_db_endpoints = list_endpoints(ydb_serverless_db)

    assert_that(hostel_db_endpoints, not_none())
    assert_that(serverless_db_endpoints, not_none())
    assert_that(serverless_db_endpoints, contains_inanyorder(*hostel_db_endpoints))
