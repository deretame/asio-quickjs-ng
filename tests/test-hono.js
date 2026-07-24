// tests/test-hono.js - Real Hono integration tests
import { Hono } from 'hono'

const app = new Hono()

app.get('/', (c) => c.text('Hello Hono!'))
app.get('/ping', (c) => c.text('pong'))

// Nested routes
const api = new Hono()
api.get('/users', (c) => c.json({ users: ['john', 'jane'] }))
api.get('/posts', (c) => c.json({ posts: 42 }))
api.get('/users/:id', (c) => c.json({ id: c.req.param('id') }))
app.route('/api', api)

app.post('/api', async (c) => {
  const body = await c.req.text()
  return c.json({ received: body.length, ok: true, type: 'json' })
})
app.post('/form', async (c) => {
  const form = await c.req.parseFormData()
  return c.json({ fields: Object.fromEntries(form), ok: true })
})
app.post('/large', async (c) => {
  const body = await c.req.arrayBuffer()
  return c.json({ size: body.byteLength, ok: true })
})

// Header tests
app.get('/headers', (c) => {
  const h = c.req.header()
  return c.json({
    host: h.host,
    userAgent: h['user-agent'],
    accept: h.accept
  })
})

app.get('/set-header', (c) => {
  c.header('X-Test', '12345')
  c.header('Content-Type', 'application/json')
  return c.json({ message: 'header set' })
})

app.get('/status', (c) => c.status(201).text('Created'))
app.get('/echo/:id', (c) => c.text(c.req.param('id')))
app.get('/query', (c) => c.json({ query: c.req.query() }))
app.get('/error', (c) => c.status(500).json({ error: 'internal' }))

console.log('Hono test server starting on port 3001')

const handler = app.fetch.bind(app)
const server = globalThis.createServer(handler)
server.listen(3001)

export { app, handler }
export default app
