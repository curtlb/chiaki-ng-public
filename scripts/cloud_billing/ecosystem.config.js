module.exports = {
  apps: [
    {
      name: "cloud-billing-udp",
      script: "cloud_billing_udp_server.py",
      interpreter: "python3",
      cwd: __dirname,
      instances: 1,
      autorestart: true,
      watch: false,
      max_memory_restart: "256M",
      env_file: ".env",
      env: {
        CS_BILLING_UDP_HOST: "0.0.0.0",
        CS_BILLING_UDP_PORT: "13750",
        CS_RETENTION_DAYS: "3",
        CS_RENEW_LEAD_MINUTES: "10",
        CS_HEARTBEAT_TIMEOUT_SEC: "180",
        CS_DEFAULT_HOURLY_PRICE: "110",
      },
    },
  ],
};
