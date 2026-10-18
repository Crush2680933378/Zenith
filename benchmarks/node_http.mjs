import http from 'node:http';
const body = Buffer.from('Hello, Benchmark!');
const json = Buffer.from('{"id":"42","name":"zenith"}');
http.createServer((req, res) => {
  if (req.url === '/hello') {
    res.writeHead(200, { 'Content-Type': 'text/plain', 'Content-Length': body.length });
    return res.end(body);
  }
  if (req.url === '/api/users/42') {
    res.writeHead(200, { 'Content-Type': 'application/json', 'Content-Length': json.length });
    return res.end(json);
  }
  res.writeHead(404); res.end();
}).listen(9106, '127.0.0.1', () => console.log('node:http on 9106'));
