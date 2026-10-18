import express from 'express';
const app = express();
app.get('/hello', (req, res) => res.type('text/plain').send('Hello, Benchmark!'));
app.get('/api/users/:id', (req, res) => res.json({ id: req.params.id, name: 'zenith' }));
app.listen(9105, '127.0.0.1', () => console.log('Express on 9105'));
