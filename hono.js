// hono.js - Real Hono v4 + streaming on asio-quickjs-ng
import { Hono } from 'hono'

const app = new Hono()

// Basic routes
app.get('/', (c) => c.text('Hello Hono on asio-quickjs-ng!'))
app.get('/ping', (c) => c.text('pong'))

// Nested routes
const api = new Hono()
api.get('/users', (c) => c.json({ users: ['john', 'jane'] }))
api.get('/posts', (c) => c.json({ posts: 42 }))
api.get('/users/:id', (c) => c.json({ id: c.req.param('id') }))
app.route('/api', api)

// POST with JSON body
app.post('/api', async (c) => {
  const body = await c.req.text()
  return c.json({ received: body.length, ok: true })
})

// Custom headers (use newResponse for full control)
app.get('/headers', (c) => {
  return c.newResponse(JSON.stringify({ ok: true }), 200, {
    'Content-Type': 'application/json',
    'X-Custom': 'hello',
    'X-Another': 'world'
  })
})

// Custom status code
app.get('/created', (c) => c.text('Created', 201))

// Empty response
app.get('/empty', (c) => c.body(null, 204))

// Streaming (chunked transfer encoding)
app.get('/stream', (c) => {
  return globalThis.stream(async (write, end) => {
    for (let i = 0; i < 5; i++) {
      await write('chunk ' + i + '\n')
    }
    await end()
  }, { contentType: 'text/plain' })
})

// SSE (Server-Sent Events)
app.get('/sse', (c) => {
  return globalThis.stream(async (write, end) => {
    for (let i = 0; i < 10; i++) {
      await write('data: message ' + i + '\n\n')
      await new Promise(r => setTimeout(r, 300))
    }
    await end()
  }, { contentType: 'text/event-stream' })
})

// Error handling
app.onError((err, c) => {
  return c.json({ error: err.message }, 500)
})

console.log('Real Hono v4 starting with streaming support')

const handler = app.fetch.bind(app)
const server = globalThis.createServer(handler)
server.listen(3000)

export { app, handler, server }
export default app
