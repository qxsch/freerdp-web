# RDP Web Client Frontend
FROM nginx:alpine

# Copy static files
COPY . /usr/share/nginx/html

# Custom nginx config to run on port 8000
COPY nginx.conf /etc/nginx/conf.d/default.conf

# Expose HTTP port
EXPOSE 8000

# Default nginx command
CMD ["nginx", "-g", "daemon off;"]
