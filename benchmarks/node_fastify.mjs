import Fastify from 'fastify';
const app = Fastify({ logger: false });
app.get('/hello', async () => 'Hello, Benchmark!');
app.get('/api/users/:id', async (req) => ({ id: req.params.id, name: 'zenith' }));
app.listen({ port: 9104, host: '127.0.0.1' }).then(() => console.log('Fastify on 9104'));
