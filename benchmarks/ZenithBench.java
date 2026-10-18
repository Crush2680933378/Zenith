// Java built-in HttpServer benchmark (JVM baseline; Spring Boot separate)
import com.sun.net.httpserver.HttpServer;
import com.sun.net.httpserver.HttpExchange;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.nio.charset.StandardCharsets;

public class ZenithBench {
    private static final byte[] HELLO = "Hello, Benchmark!".getBytes(StandardCharsets.UTF_8);
    private static final byte[] JSON = "{\"id\":\"42\",\"name\":\"zenith\"}".getBytes(StandardCharsets.UTF_8);

    public static void main(String[] args) throws Exception {
        HttpServer server = HttpServer.create(new InetSocketAddress("127.0.0.1", 9101), 0);
        server.createContext("/hello", (HttpExchange e) -> {
            e.getResponseHeaders().set("Content-Type", "text/plain");
            e.sendResponseHeaders(200, HELLO.length);
            try (OutputStream os = e.getResponseBody()) { os.write(HELLO); }
        });
        server.createContext("/api/users/42", (HttpExchange e) -> {
            e.getResponseHeaders().set("Content-Type", "application/json");
            e.sendResponseHeaders(200, JSON.length);
            try (OutputStream os = e.getResponseBody()) { os.write(JSON); }
        });
        server.setExecutor(java.util.concurrent.Executors.newFixedThreadPool(
            Runtime.getRuntime().availableProcessors()));
        server.start();
        System.out.println("Java HttpServer on 9101, cpus=" + Runtime.getRuntime().availableProcessors());
        Thread.currentThread().join();
    }
}
