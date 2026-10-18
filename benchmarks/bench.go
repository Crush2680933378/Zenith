// Go benchmarks: stdlib net/http + Gin (if gin available)
package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"os"
)

func main() {
	mode := "stdlib"
	if len(os.Args) > 1 {
		mode = os.Args[1]
	}
	_ = mode
	mux := http.NewServeMux()
	mux.HandleFunc("/hello", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/plain")
		w.Write([]byte("Hello, Benchmark!"))
	})
	mux.HandleFunc("/api/users/42", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]string{"id": "42", "name": "zenith"})
	})
	fmt.Println("Go stdlib on 9102")
	http.ListenAndServe("127.0.0.1:9102", mux)
}
