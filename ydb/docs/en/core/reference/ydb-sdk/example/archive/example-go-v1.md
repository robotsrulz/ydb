# App in Go (archived version 1)

This page contains a detailed description of the code of a [test app](https://github.com/yandex-cloud/ydb-go-sdk/tree/v1.5.1/example/basic_example_v1) that is available as part of the [v1 Go SDK](https://github.com/yandex-cloud/ydb-go-sdk/tree/v1.5.1) {{ ydb-short-name }}.

{% include [init.md](../_includes/steps/01_init.md) %}

To work with {{ ydb-short-name }} in `Go`, import the `ydb-go-sdk` driver package:

```go
import (
  // general imports
  "context"
  "path"

  // imports of ydb-go-sdk packages
  "github.com/yandex-cloud/ydb-go-sdk"
  "github.com/yandex-cloud/ydb-go-sdk/table" // to work with the table service
)
```

App code snippet for driver initialization:

```go
func (cmd *Command) Run(ctx context.Context, params cli.Parameters) error {
  dialer := &ydb.Dialer{
    DriverConfig: cmd.config(params),
    TLSConfig:    cmd.tls(),
    Timeout:      time.Second,
  }
  driver, err := dialer.Dial(ctx, params.Endpoint)
  if err != nil {
    return fmt.Errorf("dial error: %v", err)
  }
  defer driver.Close()
```

App code snippet for creating a session:

```go
tableClient := table.Client{
  Driver: driver,
}
sp := table.SessionPool{
  IdleThreshold: time.Second,
  Builder:       &tableClient,
}
defer sp.Close(ctx)
```

{% include [create_table.md](../_includes/steps/02_create_table.md) %}

To create tables, use the `Session.CreateTable()` method:

```go
func createTables(ctx context.Context, sp *table.SessionPool, prefix string) (err error) {
  err = table.Retry(ctx, sp,
    table.OperationFunc(func(ctx context.Context, s *table.Session) error {
      return s.CreateTable(ctx, path.Join(prefix, "series"),
        table.WithColumn("series_id", ydb.Optional(ydb.TypeUint64)),
        table.WithColumn("title", ydb.Optional(ydb.TypeUTF8)),
        table.WithColumn("series_info", ydb.Optional(ydb.TypeUTF8)),
        table.WithColumn("release_date", ydb.Optional(ydb.TypeUint64)),
        table.WithColumn("comment", ydb.Optional(ydb.TypeUTF8)),
        table.WithPrimaryKeyColumn("series_id"),
      )
    }),
  )
```

You can use the `Session.DescribeTable()` method to output information about the table structure and make sure that it was properly created:

```go
func describeTable(ctx context.Context, sp *table.SessionPool, path string) (err error) {
  err = table.Retry(ctx, sp,
    table.OperationFunc(func(ctx context.Context, s *table.Session) error {
      desc, err := s.DescribeTable(ctx, path)
      if err != nil {
        return err
      }
      log.Printf("\n> describe table: %s", path)
      for _, c := range desc.Columns {
        log.Printf("column, name: %s, %s", c.Type, c.Name)
      }
      return nil
    }),
  )
```

{% include [query_processing.md](../_includes/steps/04_query_processing.md) %}

To execute YQL queries, use the `Session.Execute()` method.
The SDK lets you explicitly control the execution of transactions and configure the transaction execution mode using the ```TxControl``` class.

```go
var (
  query = `
    DECLARE $seriesID AS Uint64;
    $format = DateTime::Format("%Y-%m-%d");
    SELECT
      series_id,
      title,
      $format(DateTime::FromSeconds(CAST(DateTime::ToSeconds(DateTime::IntervalFromDays(CAST(release_date AS Int16))) AS Uint32))) AS release_date
    FROM
      series
    WHERE
      series_id = $seriesID;
  `
  res *table.Result
)
readTx := table.TxControl(
  table.BeginTx(
    table.WithOnlineReadOnly(),
  ),
  table.CommitTx(),
)
err = table.Retry(ctx, sp,
  table.OperationFunc(func(ctx context.Context, s *table.Session) (err error) {
    _, res, err = s.Execute(ctx, readTx, query,
      table.NewQueryParameters(
        table.ValueParam("$seriesID", ydb.Uint64Value(1)),
      ),
      table.WithQueryCachePolicy(
        table.WithQueryCachePolicyKeepInCache(),
      ),
      table.WithCollectStatsModeBasic(),
    )
    if err != nil {
      return err
    }
    return res.Err()
  }),
)
if err != nil {
  // handling the query execution error
}
```

{% include [results_processing.md](../_includes/steps/05_results_processing.md) %}

Query results:

```go
log.Println("> select_simple_transaction:")
for res.NextSet() {
  for res.NextRow() {
    res.SeekItem("series_id")
    id := res.OUint64()

    res.NextItem()
    title := res.OUTF8()

    res.NextItem()
    date := res.OString()

    log.Printf(
      "#  SeriesID: %d , Title: %s, Date: %s\n",
      id, title, date,
    )
  }
}
```

{% include [scan_query.md](../_includes/steps/08_scan_query.md) %}

```go
var (
  query = `
    DECLARE $series AS List<UInt64>;
    SELECT series_id, season_id, title, CAST(CAST(first_aired AS Date) AS String) AS first_aired
    FROM seasons
    WHERE series_id IN $series
  `
  res *table.Result
)
err = table.Retry(ctx, sp,
  table.OperationFunc(func(ctx context.Context, s *table.Session) (err error) {
    res, err = s.StreamExecuteScanQuery(ctx, query,
      table.NewQueryParameters(
        table.ValueParam("$series",
          ydb.ListValue(
            ydb.Uint64Value(1),
            ydb.Uint64Value(10),
          ),
        ),
      ),
    )
    if err != nil {
      return err
    }
    return res.Err()
  }),
)
if err != nil {
  // handling the query execution error
}
```

Query results:

```go
log.Println("> scan_query_select:")
for res.NextStreamSet(ctx) {
  if err = res.Err(); err != nil {
    return err
  }

  for res.NextRow() {
    res.SeekItem("series_id")
    id := res.OUint64()

    res.SeekItem("season_id")
    season := res.OUint64()

    res.SeekItem("title")
    title := res.OUTF8()

    res.SeekItem("first_aired")
    date := res.OString()

    log.Printf("#  Season, SeriesId: %d, SeasonId: %d, Title: %s, Air date: %s\n", id, season, title, date)
  }
}
if err = res.Err(); err != nil {
  return err
}
return nil
```

{% include [error_handling.md](../_includes/steps/50_error_handling.md) %}

