FROM node:18-alpine

WORKDIR /app

# Only install what we need
COPY package.json package-lock.json* ./ 
RUN npm install --production

COPY . .

ENV PORT=3000
EXPOSE 3000

CMD ["node", "mock_server.js"]
