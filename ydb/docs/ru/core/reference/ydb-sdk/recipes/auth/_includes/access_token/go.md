```go
package main

import (
	"context"
	"os"
	
	"github.com/ydb-platform/ydb-go-sdk/v3"
)

func main() {
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	db, err := ydb.Open(ctx,
		os.Getenv("YDB_CONNECTION_STRING"),
		ydb.WithAccessTokenCredentials(os.Getenv("YDB_TOKEN")),
	)
	if err != nil {
		panic(err)
	}
	defer func() { 
		_ = db.Close(ctx) 
	}()
}
```
