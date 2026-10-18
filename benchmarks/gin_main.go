package main

import (
	"net/http"
	"github.com/gin-gonic/gin"
)

func main() {
	gin.SetMode(gin.ReleaseMode)
	r := gin.New() // 无中间件——纯路由开销
	r.GET("/hello", func(c *gin.Context) {
		c.String(http.StatusOK, "Hello, Benchmark!")
	})
	r.GET("/api/users/42", func(c *gin.Context) {
		c.JSON(http.StatusOK, gin.H{"id": "42", "name": "zenith"})
	})
	panic(r.Run("127.0.0.1:9103"))
}
