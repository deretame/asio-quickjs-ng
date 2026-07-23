// hono.js - Hono + http adapter 生产就绪主脚本
import { Hono } from 'hono'
import { createServer } from './src/js/http.js'

const app = new Hono()

app.get('/', (c) => c.text('Hello Hono on asio-quickjs-ng!'))
app.get('/ping', (c) => c.text('pong'))

// 嵌套路由
const api = new Hono()
api.get('/users', (c) => c.json({ users: ['john', 'jane'] }))
api.get('/posts', (c) => c.json({ posts: 42 }))
api.get('/users/:id', (c) => c.json({ id: c.req.param('id') }))
app.route('/api', api)

app.post('/api', async (c) => {
  const body = await c.req.text()
  return c.json({ received: body.length, ok: true })
})

app.get('/headers', (c) => {
  const h = c.req.header()
  return c.json({ host: h.host, userAgent: h['user-agent'] })
})
app.get('/set-header', (c) => {
  c.header('X-Test', '12345')
  c.header('Content-Type', 'application/json')
  return c.json({ message: 'header set' })
})

console.log('✅ Hono 生产就绪 - llhttp + 嵌套路由 + 请求头响应头 已集成')

const handler = app.fetch.bind(app)
const server = createServer(handler)
server.listen(3000)

export { app, handler }
export default app
