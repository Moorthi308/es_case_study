const bcrypt = require('bcryptjs');

const users = [
  { id: 1, username: 'customer1', role: 'user', passwordHash: bcrypt.hashSync(process.env.SEED_USER_PASSWORD || 'customer-password', 10) },
  { id: 2, username: 'admin1', role: 'admin', passwordHash: bcrypt.hashSync(process.env.SEED_ADMIN_PASSWORD || 'admin-password', 10) },
];

module.exports = users;
